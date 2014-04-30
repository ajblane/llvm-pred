#include "SlashShrink.h"
#include <llvm/Support/CommandLine.h>
using namespace lle;
using namespace llvm;

cl::opt<bool> markd("Mark", cl::desc("Enable Mark some code on IR"));

StringRef MarkPreserve::MarkNode = "lle.mark";

bool MarkPreserve::enabled()
{
   return ::markd;
}

void MarkPreserve::mark(Instruction* Inst)
{
   if(!Inst) return;
   if(is_marked(Inst)) return;

   LLVMContext& C = Inst->getContext();
   MDNode* N = MDNode::get(C, MDString::get(C, "mark"));
   Inst->setMetadata(MarkNode, N);
}
