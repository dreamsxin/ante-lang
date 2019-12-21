#include "args.h"
#include <map>
#include <iostream>

using namespace ante;
using namespace std;

map<string, Args> argsMap = {
    {"-check",     Args::Check},
    {"-c",         Args::CompileToObj},
    {"-r",         Args::CompileAndRun},
    {"-emit-llvm", Args::EmitLLVM},
    {"-e",         Args::Eval},
    {"-help",      Args::Help},
    {"-lib",       Args::Lib},
    {"-no-color",  Args::NoColor},
    {"-O",         Args::OptLvl},
    {"-o",         Args::OutputName},
    {"-p",         Args::Parse},
    {"-time",      Args::Time}
};

void CompilerArgs::addArg(Args &&a, string &&s){
    args.emplace_back(a, s);
}

bool CompilerArgs::hasArg(Args a) const{
    for(auto const& arg : args)
        if(arg.argTy == a)
            return true;

    return false;
}

const ante::Argument* CompilerArgs::getArg(Args a) const{
    for(auto &arg : args)
        if(arg.argTy == a)
            return &arg;

    return 0;
}

//returns true if there are no -<option> arguments.  Ignores filenames
bool CompilerArgs::empty() const{
    return args.empty();
}


enum ArgTy { None, Str, Int };

ArgTy requiresArg(Args a){
    if(a == OutputName)
        return ArgTy::Str;

    if(a == OptLvl)
        return ArgTy::Int;

    return ArgTy::None;
}

string argTyToStr(ArgTy ty){
    if(ty == ArgTy::Str) return "string";
    if(ty == ArgTy::Int) return "integer";
    return "none";
}


CompilerArgs* ante::parseArgs(int argc, const char** argv){
    CompilerArgs* ret = new CompilerArgs();

    for(int i = 1; i < argc; i++){
        if(argv[i][0] == '-'){
            try{
                Args a = argsMap.at(argv[i]);
                string s = "";

                //check to see if this argument requires an addition arg, eg -c <filename>
                ArgTy ty;
                if((ty = requiresArg(a)) != ArgTy::None){
                    if(i + 1 < argc && argv[i+1][0] != '-'){
                        s = argv[++i];
                    }else{
                        cerr << "Argument '" << argv[i] << "' requires a " << argTyToStr(ty) << " parameter.\n";
                        exit(1);
                    }
                }

                ret->addArg(move(a), move(s));
            }catch(out_of_range &r){
                cerr << "Ante: argument '" << argv[i] << "' was not recognized.\n";
                cerr << "      try -help for a list of options\n";
                exit(1);
            }

        //if it is not an option denoted by '-' it is an input file
        //options requiring their own arguments are already taken care of
        }else{
            ret->inputFiles.push_back(argv[i]);
        }
    }
    return ret;
}

