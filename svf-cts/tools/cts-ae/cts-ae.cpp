//===- cts-ae.cpp -- C TreeSitter Abstract Execution -----------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//
//===----------------------------------------------------------------------===//

/*
 * cts-ae: Abstract Execution tool using C TreeSitter frontend
 *
 * Usage: cts-ae [options] <C source files>
 *
 * This tool parses C source files using TreeSitter, builds SVFIR,
 * runs Andersen pointer analysis, then performs Abstract Interpretation
 * with buffer overflow and null pointer dereference detection.
 */

#include "CTS/CTSSVFIRBuilder.h"
#include "CTS/CTSModule.h"
#include "SVFIR/SVFIR.h"
#include "Graphs/ICFG.h"
#include "WPA/Andersen.h"
#include "AE/Svfexe/AbstractInterpretation.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"

#include <iostream>
#include <cstring>

using namespace SVF;

int main(int argc, char** argv)
{
    // Inject AE-required options (same as SVF's ae.cpp)
    int extraArgc = 3;
    char** arg_value = new char*[argc + extraArgc];
    int arg_num = 0;
    for (; arg_num < argc; ++arg_num)
        arg_value[arg_num] = argv[arg_num];
    arg_value[arg_num++] = (char*)"-model-consts=true";
    arg_value[arg_num++] = (char*)"-model-arrays=true";
    arg_value[arg_num++] = (char*)"-pre-field-sensitive=false";

    // Extract -I<path> options before passing to SVF option parser
    std::vector<std::string> includePaths;
    std::vector<char*> filteredArgs;
    for (int i = 0; i < arg_num; i++)
    {
        if (strncmp(arg_value[i], "-I", 2) == 0)
        {
            std::string arg = arg_value[i];
            if (arg.size() > 2)
                includePaths.push_back(arg.substr(2));
            else if (i + 1 < arg_num)
                includePaths.push_back(arg_value[++i]);
        }
        else
        {
            filteredArgs.push_back(arg_value[i]);
        }
    }

    // Parse SVF options, get positional args (source files)
    int filteredArgc = static_cast<int>(filteredArgs.size());
    std::vector<std::string> sourceFiles = OptionBase::parseOptions(
        filteredArgc, filteredArgs.data(),
        "C TreeSitter Abstract Execution",
        "[options] <C source files>"
    );
    delete[] arg_value;

    if (sourceFiles.empty())
    {
        std::cerr << "Error: No source files specified\n";
        return 1;
    }

    // Set include paths before building
    if (!includePaths.empty())
    {
        CTSModuleSet::getModuleSet()->setIncludePaths(includePaths);
    }

    // Step 1: Build SVFIR from C source (TreeSitter frontend)
    CTSSVFIRBuilder builder;
    SVFIR* pag = builder.build(sourceFiles);

    // Debug: dump all ICFG nodes and their statements
    // Step 2: Pointer analysis (LLVM-free, works on SVFIR)
    AndersenWaveDiff* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    CallGraph* callgraph = ander->getCallGraph();

    // Step 3: Update ICFG with call graph (for indirect calls)
    pag->getICFG()->updateCallGraph(callgraph);

    // Step 4: Abstract Interpretation
    AbstractInterpretation& ae = AbstractInterpretation::getAEInstance();
    if (Options::BufferOverflowCheck())
        ae.addDetector(std::make_unique<BufOverflowDetector>());
    if (Options::NullDerefCheck())
        ae.addDetector(std::make_unique<NullptrDerefDetector>());
    ae.runOnModule(pag->getICFG());

    // Cleanup
    AndersenWaveDiff::releaseAndersenWaveDiff();
    CTSModuleSet::releaseModuleSet();

    return 0;
}
