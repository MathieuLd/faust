/************************************************************************
 ************************************************************************
    FAUST compiler
    Copyright (C) 2003-2015 GRAME, Centre National de Creation Musicale
    ---------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 ************************************************************************
 ************************************************************************/

#include "exception.hh"
#include "Text.hh"
#include "floats.hh"
#include "global.hh"
#include "interpreter_code_container.hh"
#include "interpreter_optimizer.hh"
#include "interpreter_instructions.hh"

using namespace std;

/*
Interpreter :
 
 - a single global visitor for main and sub-containers
 - 'fSamplingFreq' and 'count' variable manually added in the IntHeap to be setup in 'instanceInit' and 'compute'
 - multiple unneeded cast are eliminated in CastNumInst
 - 'faustpower' function directly inlined in the code (see CodeContainer::pushFunction)
 - sub-containers code is 'inlined' : fields declarations (using the global visitor) and code 'classInit', and 'instanceInit' of the main container

*/

template <class T> map <string, FIRInstruction::Opcode> InterpreterInstVisitor<T>::gMathLibTable;

template <class T>
static FIRBlockInstruction<T>* getCurrentBlock()
{
    return dynamic_cast<InterpreterInstVisitor<T>*>(gGlobal->gInterpreterVisitor)->fCurrentBlock;
}

template <class T>
static InterpreterInstVisitor<T>* getInterpreterVisitor()
{
    return dynamic_cast<InterpreterInstVisitor<T>*>(gGlobal->gInterpreterVisitor);
}

template <class T>
static void setCurrentBlock(FIRBlockInstruction<T>* block)
{
    dynamic_cast<InterpreterInstVisitor<T>*>(gGlobal->gInterpreterVisitor)->fCurrentBlock = block;
}

template <class T>
InterpreterCodeContainer<T>::InterpreterCodeContainer(const string& name, int numInputs, int numOutputs)
{
    initializeCodeContainer(numInputs, numOutputs);
    fKlassName = name;
    
    // Allocate one static visitor
    if (!gGlobal->gInterpreterVisitor) {
        gGlobal->gInterpreterVisitor = new InterpreterInstVisitor<T>();
    }
    
}

template <class T>
CodeContainer* InterpreterCodeContainer<T>::createScalarContainer(const string& name, int sub_container_type)
{
    return new InterpreterScalarCodeContainer<T>(name, 0, 1, sub_container_type);
}

template <class T>
CodeContainer* InterpreterCodeContainer<T>::createContainer(const string& name, int numInputs, int numOutputs)
{
    CodeContainer* container;

    if (gGlobal->gOpenCLSwitch) {
        throw faustexception("ERROR : OpenCL not supported for Interpreter\n");
    }
    if (gGlobal->gCUDASwitch) {
        throw faustexception("ERROR : CUDA not supported for Interpreter\n");
    }

    if (gGlobal->gOpenMPSwitch) {
        throw faustexception("ERROR : OpenMP not supported for Interpreter\n");
    } else if (gGlobal->gSchedulerSwitch) {
        throw faustexception("ERROR : Scheduler not supported for Interpreter\n");
    } else if (gGlobal->gVectorSwitch) {
        throw faustexception("ERROR : Vector mode not supported for Interpreter\n");
    } else {
        container = new InterpreterScalarCodeContainer<T>(name, numInputs, numOutputs, kInt);
    }

    return container;
}

// Scalar
template <class T>
InterpreterScalarCodeContainer<T>::InterpreterScalarCodeContainer(const string& name, int numInputs, int numOutputs, int sub_container_type)
    :InterpreterCodeContainer<T>(name, numInputs, numOutputs)
{
     this->fSubContainerType = sub_container_type;
}

template <class T>
InterpreterScalarCodeContainer<T>::~InterpreterScalarCodeContainer()
{}

template <class T>
void InterpreterCodeContainer<T>::produceInternal()
{
    /// Fields generation
    generateGlobalDeclarations(gGlobal->gInterpreterVisitor);
    generateDeclarations(gGlobal->gInterpreterVisitor);
}

template <class T>
dsp_factory_base* InterpreterCodeContainer<T>::produceFactory()
{
    // Add "fSamplingFreq" variable in HEAP
    if (!fGeneratedSR) {
        fDeclarationInstructions->pushBackInst(InstBuilder::genDecStructVar("fSamplingFreq", InstBuilder::genBasicTyped(Typed::kInt)));
    }
    
    // "count" variable added to be set up later by 'compute'
    fDeclarationInstructions->pushBackInst(InstBuilder::genDecStructVar("count", InstBuilder::genBasicTyped(Typed::kInt)));
    
    // Sub containers
    mergeSubContainers();
    
    generateGlobalDeclarations(gGlobal->gInterpreterVisitor);

    generateDeclarations(gGlobal->gInterpreterVisitor);
    
    // After field declaration...
    generateSubContainers();
    
    // Rename 'sig' in 'dsp', remove 'dsp' allocation, inline subcontainers 'instanceInit' and 'fill' function call
    inlineSubcontainersFunCalls(fStaticInitInstructions)->accept(gGlobal->gInterpreterVisitor);
    
    // Keep "init_static_block"
    FIRBlockInstruction<T>* init_static_block = getCurrentBlock<T>();
    setCurrentBlock<T>(new FIRBlockInstruction<T>());
    
    // Rename 'sig' in 'dsp', remove 'dsp' allocation, inline subcontainers 'instanceInit' and 'fill' function call
    inlineSubcontainersFunCalls(fInitInstructions)->accept(gGlobal->gInterpreterVisitor);
    
    FIRBlockInstruction<T>* init_block = getCurrentBlock<T>();
    setCurrentBlock<T>(new FIRBlockInstruction<T>);
    
    generateUserInterface(gGlobal->gInterpreterVisitor);
    
    // Generates local variables declaration and setup
    generateComputeBlock(gGlobal->gInterpreterVisitor);
    
    FIRBlockInstruction<T>* compute_control_block = getCurrentBlock<T>();
    setCurrentBlock<T>(new FIRBlockInstruction<T>);

    // Generates one single scalar loop
    ForLoopInst* loop = fCurLoop->generateScalarLoop(fFullCount);
    
    loop->accept(gGlobal->gInterpreterVisitor);
    FIRBlockInstruction<T>* compute_dsp_block = getCurrentBlock<T>();
    
    // Add kReturn in generated blocks
    init_static_block->push(new FIRBasicInstruction<T>(FIRInstruction::kReturn));
    init_block->push(new FIRBasicInstruction<T>(FIRInstruction::kReturn));
    compute_control_block->push(new FIRBasicInstruction<T>(FIRInstruction::kReturn));
    compute_dsp_block->push(new FIRBasicInstruction<T>(FIRInstruction::kReturn));
    
    // Then create factory
    return new interpreter_dsp_factory_aux<T>(fKlassName, "", INTERP_FILE_VERSION,
                                                              fNumInputs, fNumOutputs,
                                                              getInterpreterVisitor<T>()->fIntHeapOffset,
                                                              getInterpreterVisitor<T>()->fRealHeapOffset,
                                                              getInterpreterVisitor<T>()->getFieldOffset("fSamplingFreq"),
                                                              getInterpreterVisitor<T>()->getFieldOffset("count"),
                                                              getInterpreterVisitor<T>()->getFieldOffset("IOTA"),
                                                              MAX_OPT_LEVEL,
                                                              produceMetadata(),
                                                              getInterpreterVisitor<T>()->fUserInterfaceBlock,
                                                              init_static_block,
                                                              init_block,
                                                              compute_control_block,
                                                              compute_dsp_block);
}

template <class T>
FIRMetaBlockInstruction* InterpreterCodeContainer<T>::produceMetadata()
{
    FIRMetaBlockInstruction* block = new FIRMetaBlockInstruction();
    
    // Add global metadata
    for (MetaDataSet::iterator i = gGlobal->gMetaDataSet.begin(); i != gGlobal->gMetaDataSet.end(); i++) {
        if (i->first != tree("author")) {
            stringstream str1, str2;
            str1 << *(i->first);
            str2 << **(i->second.begin());
            block->push(new FIRMetaInstruction(str1.str(), unquote(str2.str())));
        } else {
            for (set<Tree>::iterator j = i->second.begin(); j != i->second.end(); j++) {
                if (j == i->second.begin()) {
                    stringstream str1, str2;
                    str1 << *(i->first);
                    str2 << **j;
                    block->push(new FIRMetaInstruction(str1.str(), unquote(str2.str())));
                } else {
                    stringstream str2;
                    str2 << **j;
                    block->push(new FIRMetaInstruction("contributor", unquote(str2.str())));
                }
            }
        }
    }
    
    return block;
}