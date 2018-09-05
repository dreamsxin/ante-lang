#define NOMINMAX

#include "compapi.h"
#include "target.h"
#include <llvm/Support/TargetRegistry.h>

#if LLVM_VERSION_MAJOR >= 6
#include <llvm/Support/raw_os_ostream.h>
#endif

using namespace std;
using namespace ante;
using namespace ante::parser;

/**
 * @brief Parses a file and prints the resulting parse tree
 *
 * @param fileName The file to parse
 */
void parseFile(string &fileName){
    //parse and print parse tree
    setLexer(new Lexer(&fileName));
    yy::parser p{};
    int flag = p.parse();
    if(flag == PE_OK){
        Node* root = parser::getRootNode();
        parser::printBlock(root, 0);
        delete root;
    }else{
        //print out remaining errors
        int tok;
        yy::location loc;
        while((tok = yylexer->next(&loc)) != Tok_Newline && tok != 0);
        while(p.parse() != PE_OK && yylexer->peek() != 0);
    }
}

/**
 * @brief Outputs the help message explaining command line options.
 */
void printHelp(){
    puts("Compiler for the Ante programming language\n");
    puts("Usage: ante [options] <inputs>");
    puts("options:");
    puts("\t-c\t\tcompile to object file");
    puts("\t-o <filename>\tspecify output name");
    puts("\t-p\t\tprint parse tree");
    puts("\t-O <number>\tSet optimization level. Arg of 0 = none, 3 = all");
    puts("\t-r\t\tcompile and run");
    puts("\t-help\t\tprint this message");
    puts("\t-lib\t\tcompile as library (include all functions in binary and compile to object file)");
    puts("\t-emit-llvm\tprint llvm-IR as output");
    puts("\t-check\t\tCheck program for errors without compiling");
    puts("\t-no-color\tprint uncolored output");

    puts("\nNative target: " AN_TARGET_TRIPLE);

#if LLVM_VERSION_MAJOR >= 6
    llvm::raw_os_ostream os{std::cout};
    llvm::TargetRegistry::printRegisteredTargetsForVersion(os);
#else
    llvm::TargetRegistry::printRegisteredTargetsForVersion();
#endif
}

namespace ante {
    extern AnTypeContainer typeArena;
}

#ifndef NO_MAIN
int main(int argc, const char **argv){
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    capi::init();

    auto *args = parseArgs(argc, argv);
    if(args->hasArg(Args::Help)) printHelp();
    if(args->hasArg(Args::NoColor)) colored_output = false;

    for(auto input : args->inputFiles){
        Compiler ante{input.c_str()};
        if(args->hasArg(Args::Parse)){
            parser::printBlock(ante.getAST(), 0);
        }

        ante.processArgs(args);
        typeArena.clearDeclaredTypes();
        allCompiledModules.clear();
        allMergedCompUnits.clear();
    }

    if(args->hasArg(Args::Eval) or (args->args.empty() and args->inputFiles.empty()))
        Compiler(0).eval();

    if(yylexer)
        delete yylexer;
    delete args;

    return 0;
}
#endif
