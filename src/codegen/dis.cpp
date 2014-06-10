// Copyright (c) 2014 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "codegen/dis.h"

#include <cstdio>
#include <iostream>
#include <unordered_map>

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/IR/Mangler.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAtom.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler.h"
#include "llvm/MC/MCFunction.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCModule.h"
#include "llvm/MC/MCObjectDisassembler.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

#include "codegen/codegen.h"

namespace pyston {

PystonJITEventListener::PystonJITEventListener() {
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    llvm::InitializeNativeTargetDisassembler();

    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::getClosestTargetForJIT(err);
    assert(target);

    const llvm::StringRef triple = g.tm->getTargetTriple();
    // llvm::Triple *ltriple = new llvm::Triple(triple);
    const llvm::StringRef CPU = "";

    const llvm::MCRegisterInfo* MRI = target->createMCRegInfo(triple);
    assert(MRI);

    const llvm::MCAsmInfo* MAI = target->createMCAsmInfo(*MRI, triple);
    assert(MAI);

    const llvm::MCInstrInfo* MII = target->createMCInstrInfo();
    assert(MII);

    std::string FeaturesStr;
    const llvm::MCSubtargetInfo* STI = target->createMCSubtargetInfo(triple, CPU, FeaturesStr);
    assert(STI);

    llvm::MCObjectFileInfo* MOFI = new llvm::MCObjectFileInfo();

    llvm::MCContext* Ctx = new llvm::MCContext(MAI, MRI, MOFI);
    assert(Ctx);
    assert(Ctx->getObjectFileInfo());

    MOFI->InitMCObjectFileInfo(triple, llvm::Reloc::Default, llvm::CodeModel::Default, *Ctx);

    llvm::MCAsmBackend* TAB = target->createMCAsmBackend(*MRI, triple, CPU);
    assert(TAB);

    int AsmPrinterVariant = MAI->getAssemblerDialect(); // 0 is ATT, 1 is Intel
    IP = target->createMCInstPrinter(AsmPrinterVariant, *MAI, *MII, *MRI, *STI);
    assert(IP);

    llvm::MCCodeEmitter* CE = target->createMCCodeEmitter(*MII, *MRI, *STI, *Ctx);
    assert(CE);

    bool verbose = false;
#if LLVMREV < 208205
    llvm::MCStreamer* streamer = target->createAsmStreamer(*Ctx, llvm::ferrs(), verbose, true, true, IP, CE, TAB, true);
#else
    llvm::MCStreamer* streamer = target->createAsmStreamer(*Ctx, llvm::ferrs(), verbose, true, IP, CE, TAB, true);
#endif
    assert(streamer);
    streamer->InitSections();
    streamer->SwitchSection(Ctx->getObjectFileInfo()->getTextSection());

    asm_printer = target->createAsmPrinter(*g.tm, *streamer);
    assert(asm_printer);

    llvm::TargetOptions Options;
    llvm::TargetMachine* tmachine = target->createTargetMachine(triple, "", "", Options, llvm::Reloc::Default,
                                                                llvm::CodeModel::Default, llvm::CodeGenOpt::Default);

    // asm_printer->Mang = new llvm::Mangler(*Ctx, *tmachine->getDataLayout());
    asm_printer->Mang = new llvm::Mangler(tmachine->getDataLayout());


    DisAsm = target->createMCDisassembler(*STI, *Ctx);
    assert(DisAsm);
    MIA = target->createMCInstrAnalysis(MII);
    assert(MIA);
}

void PystonJITEventListener::NotifyFunctionEmitted(const llvm::Function& f, void* ptr, size_t size,
                                                   const llvm::JITEvent_EmittedFunctionDetails& details) {
    const llvm::MachineFunction& MF = *details.MF; //*const_cast<llvm::MachineFunction*>(details.MF);
    printf("emitted! %p %ld %s\n", ptr, size, f.getName().data());
    // MF.dump();
    // MF.print(llvm::errs());

    asm_printer->MF = &MF;
    for (llvm::MachineFunction::const_iterator it = MF.begin(); it != MF.end(); it++) {
        // it->dump();
        asm_printer->EmitBasicBlockStart(*it);
        for (llvm::MachineBasicBlock::const_instr_iterator it2 = it->instr_begin(); it2 != it->instr_end(); it2++) {
            // llvm::errs() << "dump:";
            // it2->print(llvm::errs());
            if (it2->getNumOperands() && (it2->getOperand(0).getType() == llvm::MachineOperand::MO_MCSymbol)) {
                // it2->print(llvm::errs());
                // it2->getOperand(0).print(llvm::errs());
                llvm::errs() << it2->getOperand(0).getMCSymbol()->getName() << '\n';
            } else {
                asm_printer->EmitInstruction(it2);
            }
        }
    }
    llvm::errs() << '\n';
    llvm::errs().flush();
}

void PystonJITEventListener::NotifyObjectEmitted(const llvm::ObjectImage& Obj) {
    llvm::outs() << "An object has been emitted:\n";

    llvm::error_code code;

    for (llvm::object::section_iterator I = Obj.begin_sections(), E = Obj.end_sections(); I != E;) {
        llvm::StringRef name;
        code = I->getName(name);
        assert(!code);

        uint64_t address, size;
        const char* type = "unknown";
        bool b;
        code = I->isText(b);
        assert(!code);
        if (b)
            type = "text";
        code = I->isData(b);
        assert(!code);
        if (b)
            type = "data";
        code = I->isBSS(b);
        assert(!code);
        if (b)
            type = "bss";
        code = I->isReadOnlyData(b);
        assert(!code);
        if (b)
            type = "rodata";
        code = I->getAddress(address);
        assert(!code);
        code = I->getSize(size);
        assert(!code);
        printf("Section: %s %s (%lx %lx)\n", name.data(), type, address, size);

#if LLVMREV < 200442
        I = I.increment(code);
#else
        ++I;
#endif
    }

    for (llvm::object::symbol_iterator I = Obj.begin_symbols(), E = Obj.end_symbols(); I != E;) {
        llvm::StringRef name;
        uint64_t addr, size;
        code = I->getName(name);
        assert(!code);
        code = I->getAddress(addr);
        assert(!code);
        code = I->getSize(size);
        assert(!code);
        printf("%lx %lx %s\n", addr, addr + size, name.data());
#if LLVMREV < 200442
        I = I.increment(code);
#else
        ++I;
#endif
    }

    llvm::MCObjectDisassembler* OD = new llvm::MCObjectDisassembler(*Obj.getObjectFile(), *DisAsm, *MIA);
    llvm::MCModule* Mod = OD->buildModule(true);

    // This is taken from llvm-objdump.cpp:
    uint64_t text_start = 0;
    for (llvm::MCModule::const_atom_iterator AI = Mod->atom_begin(), AE = Mod->atom_end(); AI != AE; ++AI) {
        llvm::outs() << "Atom " << (*AI)->getName() << ", starts at " << (void*)(*AI)->getBeginAddr() << ": \n";
        if ((*AI)->getName() == ".text")
            text_start = (*AI)->getBeginAddr();
        if (const llvm::MCTextAtom* TA = llvm::dyn_cast<llvm::MCTextAtom>(*AI)) {
            for (llvm::MCTextAtom::const_iterator II = TA->begin(), IE = TA->end(); II != IE; ++II) {
                llvm::outs() << "0x";
                llvm::outs().write_hex(II->Address);

                // llvm::outs() << " (+0x";
                // llvm::outs().write_hex(II->Address - text_start);
                llvm::outs() << "   (+" << II->Address - text_start;

                llvm::outs() << ")   ";
                IP->printInst(&II->Inst, llvm::outs(), "");
                llvm::outs() << "\n";
            }
        }
    }

    llvm::outs().flush();
}
}
