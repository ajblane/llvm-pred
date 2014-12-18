#include "preheader.h"
#include "Resolver.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/raw_ostream.h>

#include "ddg.h"
#include "util.h"
#include "debug.h"

using namespace std;
using namespace lle;
using namespace llvm;

static bool implicity_rule(Instruction* I, DDGraph& G);
static bool implicity_inverse_rule(Instruction* I, DDGraph& G);

char ResolverPass::ID = 0;
static RegisterPass<ResolverPass> Y("-Resolver","A Pass used to cache Resolver Result",false,false);

#ifndef NO_DEBUG
void debug_print_resolved(unordered_set<Value*>& resolved)
{
   for ( auto V : resolved){
      outs()<<*V<<"\n";
   }
}
#endif

Use* NoResolve::operator()(Value* V, ResolverBase* _UNUSED_)
{
   Instruction* I = dyn_cast<Instruction>(V);
   Assert(I,*I);
   if(!I) return NULL;
   if(isa<LoadInst>(I) || isa<StoreInst>(I))
      return &I->getOperandUse(0);
   //else if(isa<CallInst>(I))
   //   return NULL; // FIXME:not correct, first Assert, then watch which promot a call inst situation
   else
      Assert(0,*I);
   return NULL;
}

static bool indirect_access_global(Value* V)
{
   while(!isa<GlobalVariable>(V)){
      if(auto LI = dyn_cast<LoadInst>(V)) V = LI->getPointerOperand();
      else if(auto Cast = dyn_cast<CastInst>(V)) V = Cast->getOperand(0);
      else if(auto CE = dyn_cast<ConstantExpr>(V)) V = CE->getAsInstruction();
      else if(auto GEP = dyn_cast<GetElementPtrInst>(V)) V = GEP->getPointerOperand();
      else return false;
   }
   return true;
}

struct CallArgResolve{
   Use* operator()(Use* U){
      Argument* arg = findCallInstArgument(U); // adjust attribute
      if(arg && isArgumentWrite(arg) && arg->getNumUses()>0 && isa<StoreInst>(user_back(arg) )) 
         // if no nocapture and readonly, it means could write into this addr
         // FIXME: when the last inst is not store, we consider this is unsolved
         // let it return NULL if failed, to let ResolverSet use next chain
         return U;
      else
         return NULL;
   }
};

Use* UseOnlyResolve::operator()(Value* V, ResolverBase* _UNUSED_)
{
   Use* Target = NULL, *Keep = NULL;
   if(LoadInst* LI = dyn_cast<LoadInst>(V)){
      Keep = Target = &LI->getOperandUse(0);
   }else if(CastInst* CI = dyn_cast<CastInst>(V)){
      Keep = Target = &CI->getOperandUse(0);
   }else if(GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(V)) {
      Keep = Target = &GEP->getOperandUse(0); /* FIXME this is not good because
                                                GEP has many operands*/
   }else{
      Assert(0,*V);
      return NULL;
   }
   while( (Target = Target->getNext()) ){
      //seems all things who use target is after target
      auto U = Target->getUser();
      if(isa<StoreInst>(U) && U->getOperand(1) == Target->get())
         return &U->getOperandUse(0);
      else if(isa<CallInst>(U)){
         Use* R = CallArgResolve()(Target);
         if(R) return R;
      }
   }
   //if no use found, we consider this is a constant expr
   if(ConstantExpr* CE = dyn_cast<ConstantExpr>(Keep->get())){
      Instruction* TI = CE->getAsInstruction();
      return (*this)(TI, _UNUSED_);
   }

   return NULL;
}

Use* SLGResolve::operator()(Value *V, ResolverBase* _UNUSED_)
{
   if(LoadInst* LI = dyn_cast<LoadInst>(V)){
      llvm::Value* Ret = const_cast<llvm::Value*>(PI->getTrapedTarget(LI));
      if(Ret){
         StoreInst* SI = dyn_cast<StoreInst>(Ret);
         if(SI) return &SI->getOperandUse(0);
      }
   }
   return NULL;
}

/**
 * access whether a Instruction is reference global variable. 
 * if is, return the global variable
 * else return NULL
 */
static Use* access_global_variable(Instruction *I)
{
   Use* U = NULL;
	if(isa<LoadInst>(I) || isa<StoreInst>(I))
      U = &I->getOperandUse(0);
	else return NULL;
	while(ConstantExpr* CE = dyn_cast<ConstantExpr>(U->get())){
      Instruction*I = CE->getAsInstruction();
		if(isa<CastInst>(I))
         U = &I->getOperandUse(0);
		else break;
	}
	if(GetElementPtrInst* GEP = dyn_cast<GetElementPtrInst>(U->get()))
      U = &GEP->getOperandUse(GEP->getPointerOperandIndex());
	if(isa<GlobalVariable>(U->get())) return U;
	return NULL;
}

Use* SpecialResolve::operator()(Value *V, ResolverBase* RB)
{
   Use* Tg;
   if(auto LI = dyn_cast<LoadInst>(V)){
      Tg = &LI->getOperandUse(0);
   }else if(auto SI = dyn_cast<StoreInst>(V)){
      Tg = &SI->getOperandUse(0);
   }else return NULL;

   Value* U = Tg->get();
   if(auto GEP = dyn_cast<GetElementPtrInst>(U)){
      if(indirect_access_global(GEP->getPointerOperand()) &&
            GEP->getNumOperands()>2 &&
            !isa<Constant>(GEP->getOperand(1)))
         // GEP is load from a global variable array
         return Tg;
   } else if(auto Arg = dyn_cast<Argument>(U)){
      RB->ignore_cache(); /* since this doesn't point same callinst, we
                             shouldn't cache result */
      Function* F = Arg->getParent();
      CallInst* CI = RB->in_call(F);
      if(CI) return findCallInstParameter(Arg, CI);
   }
   return NULL;
}

/**
 * find where store to a global variable. 
 * require there a only store inst on this global variable.
 * NOTE!! this should replaced with UseOnlyResolve to provide a more correct
 * implement
 */
static void find_global_dependencies(const Value* GV,SmallVectorImpl<FindedDependenciesType>& Result)
{
	for(auto U = user_begin(GV),E = user_end(GV); U!=E;++U){
		Instruction* I = const_cast<Instruction*>(dyn_cast<Instruction>(*U));
		if(!I){
			find_global_dependencies(*U, Result);
			continue;
		}
		if(isa<StoreInst>(I)){
			Result.push_back(make_pair(MemDepResult::getDef(I),I->getParent()));
		}
	}
}

void MDAResolve::find_dependencies( Instruction* I, const Pass* P,
      SmallVectorImpl<FindedDependenciesType>& Result, NonLocalDepResult* NLDR)
{
   MemDepResult d;
   BasicBlock* SearchPos;
   Instruction* ScanPos;
   MemoryDependenceAnalysis& MDA = P->getAnalysis<MemoryDependenceAnalysis>();
   AliasAnalysis& AA = P->getAnalysis<AliasAnalysis>();

   if(Use* GV = access_global_variable(I)){
      find_global_dependencies(GV->get(), Result);
      return;
   }

   AliasAnalysis::Location Loc;
   if(LoadInst* LI = dyn_cast<LoadInst>(I)){
      Loc = AA.getLocation(LI);
   }else if(StoreInst* SI = dyn_cast<StoreInst>(I))
      Loc = AA.getLocation(SI);
   else
      assert(0);

   if(!NLDR){
      SearchPos = I->getParent();
      d = MDA.getPointerDependencyFrom(Loc, isa<LoadInst>(I), I, SearchPos, I);
      ScanPos = d.getInst();
   }else{
      ScanPos = NLDR->getResult().getInst();
      SearchPos = NLDR->getBB();
      d = NLDR->getResult();
      //we have already visit this BB;
      if(find_if(Result.begin(),Result.end(),[&SearchPos](FindedDependenciesType& f){
               return f.second == SearchPos;}
               ) != Result.end()){
         return;
      }
   }

   while(ScanPos){
      //if isDef, that's what we want
      //if isNonLocal, record BasicBlock to avoid visit twice
      if(d.isDef()||d.isNonLocal())
         Result.push_back(make_pair(d,SearchPos));
      if(d.isDef() && isa<StoreInst>(d.getInst()) )
         return;

      d = MDA.getPointerDependencyFrom(Loc, isa<LoadInst>(I), ScanPos, SearchPos, I);
      ScanPos = d.getInst();
   }

   //if local analysis result is nonLocal 
   //we didn't found a good result, so we continue search
   if(d.isNonLocal()){
      //we didn't record in last time
      Result.push_back(make_pair(d,SearchPos));

      SmallVector<NonLocalDepResult,32> NonLocals;

      MDA.getNonLocalPointerDependency(Loc, isa<LoadInst>(I), SearchPos, NonLocals);
      for(auto r : NonLocals){
         find_dependencies(I, P, Result, &r);
      }
   }else if(d.isNonFuncLocal()){
      //doing some thing here
      Resolver<NoResolve> R;
      bool has_argument = R.resolve_if(I->getOperand(0), [](Value* V){
               return isa<Argument>(V);
            });
      if(has_argument)
         Result.push_back(make_pair(d,SearchPos));
   }
}

Use* MDAResolve::operator()(llvm::Value * V, ResolverBase* _UNUSED_)
{
   Assert(pass,"Not initial with pass");

   SmallVector<FindedDependenciesType, 64> Result;
   Instruction* I = dyn_cast<Instruction>(V);

   if(!I) return NULL;

   find_dependencies(I, pass, Result);

   for(auto R : Result){
      Instruction* Ret = R.first.getInst();
      if(R.first.isDef() && Ret != I)
         return &R.first.getInst()->getOperandUse(0); //FIXME: 应该寻找使用的是哪个参数,而不是直接认为是0
      if(R.first.isNonFuncLocal())
         return NULL;
   }
   return NULL;
}

list<Value*> ResolverBase::direct_resolve( Value* V, unordered_set<Value*>& resolved, function<void(Value*)> lambda)
{
   std::list<llvm::Value*> unresolved;

   if(resolved.find(V) != resolved.end())
      return unresolved;
   if(isa<Argument>(V)){
      unresolved.push_back(V);
   } else if(isa<Constant>(V)){
      resolved.insert(V);
      lambda(V);
   }else if(Instruction* I = dyn_cast<Instruction>(V)){
      if(isa<LoadInst>(I)){
         unresolved.push_back(I);
      }else{
         resolved.insert(I);
         lambda(I);
         uint N = isa<StoreInst>(I)?1:I->getNumOperands();/*if isa store inst,
                                                         we only solve arg 0*/
         for(uint i=0;i<N;i++){
            Value* R = I->getOperand(i);
            auto rhs = direct_resolve(R, resolved, lambda);
            unresolved.insert(unresolved.end(), rhs.begin(), rhs.end());
         }
      }
   }
   return unresolved;
}

void ResolverBase::_empty_handler(llvm::Value *)
{
}

CallInst* ResolverBase::in_call(Function *F) const
{
   auto Ite = find_if(call_stack.begin(), call_stack.end(), [F](CallInst* CI){
         return CI->getCalledFunction() == F;
         });
   if(Ite!=call_stack.end()) return *Ite;
   CallInst* only;
   unsigned call_count = count_if(user_begin(F), user_end(F), [&only](User* U){
         return (only = dyn_cast<CallInst>(U)) > 0;
         });
   if(call_count==1) return only;
   return NULL;
}

ResolveResult ResolverBase::resolve(llvm::Value* V, std::function<void(Value*)> lambda)
{
   call_stack.clear(); // clear call_stack
   std::unordered_set<Value*> resolved;
   std::list<Value*> unsolved;
   std::unordered_map<Value*, Use*> partial;

   unsolved.push_back(NULL); /* always make sure Ite wouldn't point to end();
      因为在一开始的时候,如果没有这句,Ite指向end(),然后又在end后追加数据,造成
      Ite失效.  */

   auto Ite = unsolved.begin();
   Value* next = V; // wait to next resolved;

   while(next || *Ite != NULL){
      if(stop_resolve) break;
      if(next){
         list<Value*> mid = direct_resolve(next, resolved, lambda);
         unsolved.insert(--unsolved.end(),mid.begin(),mid.end()); 
            // insert before NULL, makes NULL always be tail
         next = NULL;
         if(*Ite == NULL) advance(Ite, -mid.size()); /* *Ite == NULL means *Ite points 
            tail, and we insert before it, we should put forware what we inserted */
         continue;
      }

      Use* res = NULL;
      Instruction* I = dyn_cast<Instruction>(*Ite);
      //FIXME: may be we should directly drop A to deep_resolve(A)
      if(!I){
         ++Ite;
         continue;
      }

      res = deep_resolve(I);
      if(!res || res->getUser()!= *Ite/* if returns self, it means unknow
                                         answer, we shouldn't insert partial */
            ){ 
         partial.insert(make_pair(*Ite,res)); 
         if(!res){
            ++Ite;
            continue;
         }
      }
      User* U = res->getUser();

      resolved.insert(I); // original is resolved;
      lambda(I);
      resolved.insert(U);
      lambda(U);
      Ite = unsolved.erase(Ite);

recursive:
      if(isa<StoreInst>(U) || isa<LoadInst>(U)){
         next = res->get();
      }else if(auto CI = dyn_cast<CallInst>(U)){
         if(CI->getCalledFunction() != I->getParent()->getParent()){ 
            // original Instruction isn't in the function called, means param
            // direction is out, that is, put a param to called function, and
            // write out result into it.
            Argument* arg = findCallInstArgument(res);
            if(arg){
               // arg == NULL, maybe it calls a library function
               partial.insert(make_pair(arg,&user_back(arg)->getOperandUse(0)));
               next = user_back(arg);
               call_stack.push_back(CI);
            }
         }else{
            // original Instruction is in same function called, means param
            // direction is in, that is, the outter put a param to called
            // function, and we(in function body) read the value and do caculate
            if(!isa<AllocaInst>(res->get())){
               next = res->get();
               continue;
            }

            while((res = res->getNext())){
               Use* R = NULL;
               User* Ur = res->getUser();
               if(isa<CallInst>(Ur)){
                  R = CallArgResolve()(res);
                  if(R){
                     U = Ur;
                     goto recursive; // goto above siatuation
                  }
               }else if(isa<LoadInst>(Ur) || isa<StoreInst>(Ur)){
                  next = Ur;
                  partial.insert(make_pair(res->get(),res));
                  break;
               }else
                  AssertRuntime(0, "");
            }
         }
      }else
         next = U;
   }

   unsolved.pop_back(); // remove last NULL
   return make_tuple(resolved, unsolved, partial);
}

bool ResolverBase::resolve_if(Value *V, function<bool (Value *)> lambda)
{
   stop_resolve = false;
   bool ret = false;

   resolve(V,[&lambda, &ret, this](Value* v){
            if(lambda(v)){
               this->stop_resolve = true;
               ret = true;
            }
         });
   stop_resolve = false;
   return ret;
}

void ResolverPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const
{
   AU.setPreservesAll();
}

ResolverPass::~ResolverPass(){
   for(auto I : impls)
      delete I.second;
}


ResolveEngine::ResolveEngine()
{
   implicity_rule = ::implicity_rule;
}

void ResolveEngine::do_solve(DDGraph& G, CallBack& C)
{
   while(Use* un = G.popUnsolved()){
      if(C(un)) continue;
      for(auto r = rules.rbegin(), e = rules.rend(); r!=e; ++r){
         if((*r)(un, G)) break;
      }
   }
}

DDGraph ResolveEngine::resolve(Use& U, CallBack C)
{
   DDGraph G;
   G.addUnsolved(U);
   do_solve(G, C);
   G.setRoot(&U);
   return G;
}

DDGraph ResolveEngine::resolve(Instruction* I, CallBack C)
{
   DDGraph G;
   implicity_rule(I, G);
   do_solve(G, C);
   G.setRoot(I);
   return G;
}

Value* ResolveEngine::find_store(Use& Tg, CallBack C)
{
   Value* ret = NULL;
   resolve(Tg, [&ret, &C](Use* U){
         User* Ur = U->getUser();
         if(isa<StoreInst>(Ur))
            ret=Ur->getOperand(0);
         return C(U);
      });
   return ret;
}

Value* ResolveEngine::find_visit(Use& Tg, CallBack C)
{
   Value* ret = NULL;
   User* TgUr = Tg.getUser();
   resolve(Tg, [&ret, &C, TgUr](Use* U){
         User* Ur = U->getUser();
         if(Ur == TgUr) return C(U);
         if(isa<LoadInst>(Ur))
            ret=Ur;
         else if(CallInst* CI = dyn_cast<CallInst>(Ur)){//call inst also is a kind of visit
            if(Function* F = dyn_cast<Function>(castoff(CI->getCalledValue())))
               if(!F->getName().startswith("llvm."))//llvm call should ignore
                  ret=CI;
         }
         return C(U);
      });
   return ret;
}

//===========================RESOLVE RULES======================================//


static bool useonly_rule_(Use* U, DDGraph& G)
{
   User* V = U->getUser();
   Use* Tg = U;
   if(isa<LoadInst>(V))
      Tg = U; // if is load, find after use
   else if(isa<GetElementPtrInst>(V))
      Tg = &*V->use_begin(); // if is GEP, find all use @example : %5 = [GEP] ; store %6, %5
   else return false;

   do{
      //seems all things who use target is after target
      auto V = Tg->getUser();
      if(isa<StoreInst>(V) && V->getOperand(1) == Tg->get()){
         DEBUG(errs()<<"UseOnly Rule:"<<*V<<"\n");
         G.addSolved(U, V->getOperandUse(0));
         return true;
      }
   } while( (Tg = Tg->getNext()) );
   return false;
}
static bool direct_rule_(Use* U, DDGraph& G)
{
   Value* V = U->get();
   if(auto I = dyn_cast<Instruction>(V)){
      G.addSolved(U, I->op_begin(), I->op_end());
      return true;
   }else if(auto C = dyn_cast<Constant>(V)){
      G.addSolved(U, C);
      return true;
   }
   return false;
}
static bool direct_inverse_rule_(Use* U, DDGraph& G)
{
   Value* V = U->getUser();
   auto uses = to_vector(V->use_begin(), V->use_end());
   G.addSolved(U, uses.rbegin(), uses.rend());
   return true;
}
static bool use_inverse_rule_(Use* U, DDGraph& G)
{
   Value* V = U->get();
   auto uses = to_vector(V->use_begin(), find_iterator(*U));
   G.addSolved(U, uses.rbegin(), uses.rend());
   return true;
}
static bool implicity_rule(Instruction* I, DDGraph& G)
{
   if(isa<StoreInst>(I)){
      G.addSolved(I, I->getOperandUse(0));
      return false;
   }else{
      G.addSolved(I, I->op_begin(), I->op_end());
      return true;
   }
}
static bool implicity_inverse_rule(Instruction* I, DDGraph& G)
{
   auto uses = to_vector(I->use_begin(), I->use_end());
   G.addSolved(I, uses.rbegin(), uses.rend());
   return true;
}
static bool gep_rule_(Use* U, DDGraph& G)
{
   // if isa GEP, means we already have solved before.
   if(isa<GetElementPtrInst>(U->getUser())) return false;
   Type* Ty = U->get()->getType();
   if(Ty->isPointerTy()) Ty = Ty->getPointerElementType();
   // only struct or array can have GEP
   if(!Ty->isStructTy() && !Ty->isArrayTy()) return false;
   Use* Tg = U;
   bool ret = false;
   while( (Tg = Tg->getNext()) ){
      auto V = Tg->getUser();
      if(isa<GetElementPtrInst>(V) && V->getOperand(0) == Tg->get() ){
         // add all GEP to Unsolved
         DEBUG(errs()<<"GEP Rule:"<<*V<<"\n");
         G.addSolved(U, V->getOperandUse(0));
         ret = true;
      }
   }
   return ret;
}
void InitRule::operator()(ResolveEngine& RE)
{
   bool& init = this->initialized;
   auto& r = rule;
   ResolveEngine::SolveRule S = [&init, &r](Use* U, DDGraph& G){
      if(!init){
         init = true;
         return r(U,G);
      }
      return false;
   };
   RE.addRule(S);
}

void MDARule::operator()(ResolveEngine &RE)
{
   MemoryDependenceAnalysis& MDA = this->MDA;
   AliasAnalysis&AA = this->AA;
   ResolveEngine::SolveRule S = [&MDA,&AA](Use* U, DDGraph& G){
      LoadInst* LI = dyn_cast<LoadInst>(U->getUser());
      if(LI==NULL) return false;
      MemDepResult Dep = MDA.getDependency(LI);
      //Def: 定义, 可能是Alloca, 或者...
      //Clobber: 改写, Load依赖的Store, Store改写了内存
      if(!Dep.isDef() && !Dep.isClobber()) return false;
      AliasAnalysis::Location Loc = AA.getLocation(LI);
      //inspired from DeadStoreElimination.cpp
      if(!Loc.Ptr) return false;
      while(Dep.isDef() || Dep.isClobber()){
         Instruction* DepWrite = Dep.getInst();
         Dep = MDA.getPointerDependencyFrom(Loc, 1, DepWrite, LI->getParent());
      }
   };
   RE.addRule(S);
}

void ResolveEngine::base_rule(ResolveEngine& RE)
{
   RE.addRule(SolveRule(direct_rule_));
   RE.implicity_rule = ::implicity_rule;
}
void ResolveEngine::ibase_rule(ResolveEngine& RE)
{
   RE.addRule(SolveRule(direct_inverse_rule_));
   RE.implicity_rule = ::implicity_inverse_rule;
}
const ResolveEngine::CallBack ResolveEngine::always_false = [](Use* U){ return false; };
const ResolveEngine::SolveRule ResolveEngine::useonly_rule = useonly_rule_;
const ResolveEngine::SolveRule ResolveEngine::gep_rule = gep_rule_;
const ResolveEngine::SolveRule ResolveEngine::iuse_rule = use_inverse_rule_;

//===============================RESOLVE RULES END===============================//
