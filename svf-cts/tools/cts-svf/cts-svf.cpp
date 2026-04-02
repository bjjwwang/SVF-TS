//===- cts-svf.cpp -- C TreeSitter SVF analysis tool -----------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//
//===----------------------------------------------------------------------===//

/*
 * cts-svf: Main tool for C TreeSitter-based SVF analysis
 *
 * Usage: cts-svf [options] <source files>
 *
 * This tool parses C source files using TreeSitter, builds
 * SVFIR (PAG) and ICFG, and optionally dumps them as DOT files.
 */

#include "CTS/CTSSVFIRBuilder.h"
#include "CTS/CTSModule.h"
#include "SVFIR/SVFIR.h"
#include "Graphs/ICFG.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <map>
#include <sys/stat.h>

using namespace SVF;

static bool createDirectory(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
    {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

static std::string getBasename(const std::string& path)
{
    size_t pos = path.find_last_of("/\\");
    std::string filename = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    size_t dotPos = filename.find_last_of('.');
    if (dotPos != std::string::npos)
    {
        filename = filename.substr(0, dotPos);
    }
    return filename;
}

void printUsage(const char* programName)
{
    std::cout << "Usage: " << programName << " [options] <source files>\n"
              << "\n"
              << "C TreeSitter-based SVF analysis tool\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help         Show this help message\n"
              << "  -v, --verbose      Enable verbose output\n"
              << "  -o, --output-dir   Output directory for dumps (default: current dir)\n"
              << "  -I<path>           Add include search path for #include resolution\n"
              << "  --dump-pag         Dump the PAG to DOT file\n"
              << "  --dump-icfg        Dump the ICFG to DOT file\n"
              << "  --dump-all         Dump PAG and ICFG\n"
              << "  --dump-stmts       Dump all SVFStmts per function (text)\n"
              << "\n"
              << "Examples:\n"
              << "  " << programName << " main.c\n"
              << "  " << programName << " --dump-pag test.c\n"
              << "  " << programName << " -o output/ --dump-all test.c\n"
              << std::endl;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printUsage(argv[0]);
        return 1;
    }

    std::vector<std::string> sourceFiles;
    std::vector<std::string> includePaths;
    std::string outputDir = ".";
    bool verbose = false;
    bool dumpPAG = false;
    bool dumpICFG = false;
    bool dumpStmts = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            printUsage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
        {
            verbose = true;
        }
        else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output-dir") == 0)
        {
            if (i + 1 < argc)
            {
                outputDir = argv[++i];
            }
            else
            {
                std::cerr << "Error: -o requires an argument" << std::endl;
                return 1;
            }
        }
        else if (strncmp(argv[i], "-I", 2) == 0)
        {
            std::string arg = argv[i];
            if (arg.size() > 2)
                includePaths.push_back(arg.substr(2));
            else if (i + 1 < argc)
                includePaths.push_back(argv[++i]);
        }
        else if (strcmp(argv[i], "--dump-pag") == 0)
        {
            dumpPAG = true;
        }
        else if (strcmp(argv[i], "--dump-icfg") == 0)
        {
            dumpICFG = true;
        }
        else if (strcmp(argv[i], "--dump-all") == 0)
        {
            dumpPAG = true;
            dumpICFG = true;
        }
        else if (strcmp(argv[i], "--dump-stmts") == 0)
        {
            dumpStmts = true;
        }
        else if (argv[i][0] != '-')
        {
            sourceFiles.push_back(argv[i]);
        }
        else
        {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (sourceFiles.empty())
    {
        std::cerr << "Error: No source files specified" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    if ((dumpPAG || dumpICFG) && outputDir != ".")
    {
        if (!createDirectory(outputDir))
        {
            std::cerr << "Error: Cannot create output directory: " << outputDir << std::endl;
            return 1;
        }
    }

    std::string baseName = getBasename(sourceFiles[0]);

    if (verbose)
    {
        std::cout << "=== C TreeSitter SVF Analysis ===" << std::endl;
        std::cout << "Processing " << sourceFiles.size() << " source file(s):" << std::endl;
        for (const auto& file : sourceFiles)
        {
            std::cout << "  - " << file << std::endl;
        }
        std::cout << std::endl;
    }

    try
    {
        // Set include paths before building
        if (!includePaths.empty())
        {
            CTSModuleSet::getModuleSet()->setIncludePaths(includePaths);
        }

        // Build SVFIR from C source files
        CTSSVFIRBuilder builder;
        SVFIR* pag = builder.build(sourceFiles);

        if (verbose)
        {
            std::cout << "PAG nodes: " << pag->getPAGNodeNum() << std::endl;
            std::cout << "PAG edges: " << pag->getPAGEdgeNum() << std::endl;
            std::cout << std::endl;
        }

        // Dump PAG as DOT
        if (dumpPAG)
        {
            std::string pagPath = outputDir + "/" + baseName + "_pag";
            pag->dump(pagPath);
            std::cout << "PAG dump written to: " << pagPath << ".dot" << std::endl;
        }

        // Dump ICFG as DOT
        if (dumpICFG)
        {
            ICFG* icfg = pag->getICFG();
            if (icfg)
            {
                std::string icfgPath = outputDir + "/" + baseName + "_icfg";
                icfg->dump(icfgPath);
                std::cout << "ICFG dump written to: " << icfgPath << ".dot" << std::endl;
            }
        }

        // Dump SVFStmts per function
        if (dumpStmts)
        {
            ICFG* icfg = pag->getICFG();
            std::map<std::string, std::vector<std::string>> funcStmts;
            for (auto it = icfg->begin(), eit = icfg->end(); it != eit; ++it)
            {
                const ICFGNode* node = it->second;
                const auto& stmts = node->getSVFStmts();
                if (stmts.empty()) continue;
                const FunObjVar* fun = node->getFun();
                std::string funName = fun ? fun->getName() : "<global>";
                for (const SVFStmt* stmt : stmts)
                    funcStmts[funName].push_back(stmt->toString());
            }
            for (auto& kv : funcStmts)
            {
                std::cout << "[" << kv.first << "]" << std::endl;
                for (auto& s : kv.second)
                    std::cout << "  " << s << std::endl;
            }
        }

        if (verbose)
        {
            std::cout << "=== Analysis Complete ===" << std::endl;
        }

        // Cleanup
        CTSModuleSet::releaseModuleSet();

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
