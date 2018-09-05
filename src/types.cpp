#include <types.h>
using namespace std;
using namespace llvm;
using namespace ante::parser;

namespace ante {

char getBitWidthOfTypeTag(const TypeTag ty){
    switch(ty){
        case TT_I8:  case TT_U8: case TT_C8:  return 8;
        case TT_I16: case TT_U16: case TT_F16: return 16;
        case TT_I32: case TT_U32: case TT_F32: case TT_C32: return 32;
        case TT_I64: case TT_U64: case TT_F64: return 64;
        case TT_Isz: case TT_Usz: return AN_USZ_SIZE;
        case TT_Bool: return 8;

        case TT_Ptr:
        case TT_Function:
        case TT_MetaFunction:
        case TT_FunctionList: return AN_USZ_SIZE;

        default: return 0;
    }
}

TypedValue FunctionCandidates::getAsTypedValue(llvm::LLVMContext *c, std::vector<std::shared_ptr<FuncDecl>> &ca, TypedValue o){
    return {(Value*)new FunctionCandidates(c, ca, o),
        AnType::getPrimitive(TT_FunctionList)};
}



/*
 *  Returns the TypeNode* value of a TypedValue of type TT_Type
 */
AnType* extractTypeValue(const TypedValue &tv){
    auto zext = dyn_cast<ConstantInt>(tv.val)->getZExtValue();
    return (AnType*) zext;
}


/*
 *  Checks to see if a type is valid to be used.
 *  To be valid the type must:
 *      - Not be recursive (contain no references to
 *        itself that are not behind a pointer)
 *      - Contain no typevars that are not declared
 *        within the rootTy's params
 *      - Contain only data types that have been declared
 */
void validateType(Compiler *c, const AnType *tn, const DataDeclNode *rootTy){
    if(!tn) return;

    if(tn->typeTag == TT_Data or tn->typeTag == TT_TaggedUnion){
        auto *dataTy = try_cast<AnDataType>(tn);

        if(dataTy->isStub()){
            if(dataTy->name == rootTy->name){
                c->compErr("Recursive types are disallowed, wrap the type in a pointer instead", rootTy->loc);
            }

            c->compErr("Type "+dataTy->name+" has not been declared", rootTy->loc);
        }

        for(auto *t : dataTy->extTys)
            validateType(c, t, rootTy);

    }else if(tn->typeTag == TT_Tuple){
        auto *agg = try_cast<AnAggregateType>(tn);
        for(auto *ext : agg->extTys){
            validateType(c, ext, rootTy);
        }
    }else if(tn->typeTag == TT_Array){
        auto *arr = try_cast<AnArrayType>(tn);
        validateType(c, arr->extTy, rootTy);
    }else if(tn->typeTag == TT_Ptr or tn->typeTag == TT_Function or tn->typeTag == TT_MetaFunction){
        return;

    }else if(tn->typeTag == TT_TypeVar){
        auto *tvt = try_cast<AnTypeVarType>(tn);
        auto *binding = c->lookupTypeVar(tvt->name);
        if(binding){
            return validateType(c, binding, rootTy);
        }

        //Typevar not found, if its not in the rootTy's params, then it is unbound
        for(auto &p : rootTy->generics){
            if(p->typeName == tvt->name) return;
        }

        c->compErr("Lookup for "+tvt->name+" not found", rootTy->loc);
    }
}

void validateType(Compiler *c, const AnType *tn, const AnDataType *dt){
    auto fakeLoc = mkLoc(mkPos(0, 0, 0), mkPos(0, 0, 0));
    auto *ddn = new DataDeclNode(fakeLoc, dt->name, 0, 0, false);

    for(auto &g : dt->generics){
        auto *tv = mkAnonTypeNode(TT_TypeVar);
        tv->typeName = g.typeVarName;
        ddn->generics.emplace_back(tv);
    }

    validateType(c, tn, ddn);
    ddn->generics.clear();
    delete ddn;
}


Result<size_t, string> AnType::getSizeInBits(Compiler *c, string *incompleteType, bool force) const{
    size_t total = 0;

    if(isPrimitiveTypeTag(this->typeTag))
        return getBitWidthOfTypeTag(this->typeTag);

    if(auto *dataTy = try_cast<AnDataType>(this)){
        if(dataTy->isStub()){
            if(incompleteType && dataTy->name == *incompleteType){
                cerr << "Incomplete type " << anTypeToColoredStr(this) << endl;
                throw new IncompleteTypeError();
            }

            return "Type " + anTypeToStr(this) + " has not been declared\n";
        }

        for(auto *ext : dataTy->extTys){
            auto val = ext->getSizeInBits(c, incompleteType, force);
            if(!val) return val;
            total += val.getVal();
        }

    // function & metafunction are aggregate types but have different sizes than
    // a tuple so this case must be checked for before AnAggregateType is
    }else if(typeTag == TT_Ptr or typeTag == TT_Function or typeTag == TT_MetaFunction){
        return AN_USZ_SIZE;

    }else if(auto *tup = try_cast<AnAggregateType>(this)){
        for(auto *ext : tup->extTys){
            auto val = ext->getSizeInBits(c, incompleteType, force);
            if(!val) return val;
            total += val.getVal();
        }

    }else if(auto *arr = try_cast<AnArrayType>(this)){
        auto val = arr->extTy->getSizeInBits(c, incompleteType, force);
        if(!val) return val;
        return arr->len * val.getVal();

    }else if(auto *tvt = try_cast<AnTypeVarType>(this)){
        auto *binding = c->lookupTypeVar(tvt->name);
        if(binding){
            if(binding == tvt){
                return "Typevar " + tvt->name + " refers to itself, cannot calculate size in bits";
            }
            //possible infinite loop for mutally recursive typevars
            return binding->getSizeInBits(c, incompleteType, force);
        }

        if(force) return AN_USZ_SIZE;
        else return "Lookup for typevar " + tvt->name + " not found";
    }

    return total;
}


size_t hashCombine(size_t l, size_t r){
    return l ^ (r + AN_HASH_PRIME + (l << 6) + (l >> 2));
}


/**
* @brief Searches for the suggested binding of a typevar
*
* @param s Name of the typevar to search for a binding for
*
* @return The binding if found, nullptr otherwise
*/
const TypeBinding* findBindingFor(GenericTypeParam const& param, vector<TypeBinding> const& bindings){
    for(auto &b : bindings){
        if(b.matches(param))
            return &b;
    }
    return nullptr;
}


vector<TypeBinding> filterMatchingBindings(const AnDataType *dt, vector<TypeBinding> const& bindings){
    vector<TypeBinding> matches;
    for(auto &g : dt->generics){
        auto *b = findBindingFor(g, bindings);
        if(b){
            matches.push_back(*b);
        }
    }
    return matches;
}

string toLlvmTypeName(const AnDataType *dt){
    auto &typeArgs = dt->boundGenerics;
    auto &baseName = dt->name;

    if(typeArgs.empty())
        return baseName;

    string name = baseName + "<";
    for(auto &b : typeArgs){
        if(AnDataType *ext = try_cast<AnDataType>(b.getBinding()))
            name += toLlvmTypeName(ext);
        else
            name += anTypeToStr(b.getBinding());

        if(&b != &typeArgs.back())
            name += ",";
    }
    return name == baseName + "<" ? baseName : name+">";
}

Type* updateLlvmTypeBinding(Compiler *c, AnDataType *dt, bool force){
    //create an empty type first so we dont end up with infinite recursion
    bool isPacked = dt->typeTag == TT_TaggedUnion;
    auto* structTy = dt->llvmType ? (StructType*)dt->llvmType
        : StructType::create(*c->ctxt, {}, toLlvmTypeName(dt), isPacked);

    dt->llvmType = structTy;

    if(dt->isGeneric && !force){
        cerr << "Type " << anTypeToStr(dt) << " is generic and cannot be translated.\n";
        c->errFlag = true;
    }

    AnType *ext = dt;
    if(dt->typeTag == TT_TaggedUnion)
        ext = getLargestExt(c, dt, force);

    vector<Type*> tys;
    if(auto *aggty = try_cast<AnAggregateType>(ext)){
        for(auto *e : aggty->extTys){
            auto *llvmTy = c->anTypeToLlvmType(e, force);
            if(!llvmTy->isVoidTy())
                tys.push_back(llvmTy);
        }
    }else{
        tys.push_back(c->anTypeToLlvmType(ext, force));
    }

    structTy->setBody(tys, isPacked);
    return structTy;
}

/*
 *  Generics can be stored and bound in two ways
 *    1. Stored as a map from name of typevar -> type bound to
 *       - Handled by this function
 *       - This is the format returned by a typeEq type check if it
 *         indicates the check would only be a success if those typevars
 *         are bound to the given types.  This is TypeCheckResult::SuccessWithTypeVars
 *    2. Stored as a vector of ordered bound types.
 *       - The ordering of type vars in this format is matched to the order of the
 *         declaration of generics when the datatype was first declared.
 *         Eg. with type Map<'k,'v> = ... and bindings {Str, i32}, 'k is bound to
 *         Str and 'v is bound to i32.
 *       - This is the format used internally by TypeNodes and DataTypes.
 *       - Before being bound in bindGenericToType this representation must be
 *         converted to the first beforehand, and to do that it needs the DataType
 *         to match the typevar ordering with.  The second function below handles
 *         this conversion
 */
AnType* bindGenericToType(Compiler *c, AnType *tn, vector<TypeBinding> const& bindings){
    if(!tn->isGeneric){
        return tn;
    }else if(bindings.empty()){
        return tn;
    }

    if(auto *dty = try_cast<AnDataType>(tn)){
        return AnDataType::getVariant(c, dty, bindings);

    }else if(auto *tv = try_cast<AnTypeVarType>(tn)){
        for(auto& b : bindings){
            if(b.matches(tv->name)){
                return b.getBinding();
            }
        }
        cerr << "warning: unbound type var " << tv->name << " in binding\n";
        return tv;

    }else if(auto *agg = try_cast<AnAggregateType>(tn)){
        vector<AnType*> exts;
        exts.reserve(agg->extTys.size());
        for(auto *e : agg->extTys){
            exts.push_back(bindGenericToType(c, e, bindings));
        }
        return AnAggregateType::get(tn->typeTag, exts);

    }else if(auto *ptr = try_cast<AnPtrType>(tn)){
        auto *ty = bindGenericToType(c, ptr->extTy, bindings);
        return AnPtrType::get(ty);

    }else if(auto *arr = try_cast<AnArrayType>(tn)){
        auto *ty = bindGenericToType(c, arr->extTy, bindings);
        return AnArrayType::get(ty, arr->len);

    }else{
        return tn;
    }
}


/*
 *  Checks for, and implicitly widens an integer or float type.
 *  The original value of num is returned if no widening can be performed.
 */
TypedValue Compiler::implicitlyWidenNum(TypedValue &num, TypeTag castTy){
    bool lIsInt = isIntTypeTag(num.type->typeTag);
    bool lIsFlt = isFPTypeTag(num.type->typeTag);

    if(lIsInt or lIsFlt){
        bool rIsInt = isIntTypeTag(castTy);
        bool rIsFlt = isFPTypeTag(castTy);
        if(!rIsInt && !rIsFlt){
            cerr << "castTy argument of implicitlyWidenNum must be a numeric primitive type\n";
            exit(1);
        }

        int lbw = getBitWidthOfTypeTag(num.type->typeTag);
        int rbw = getBitWidthOfTypeTag(castTy);
        Type *ty = typeTagToLlvmType(castTy, *ctxt);

        //integer widening
        if(lIsInt && rIsInt){
            if(lbw <= rbw){
                return TypedValue(
                    builder.CreateIntCast(num.val, ty, !isUnsignedTypeTag(num.type->typeTag)),
                    AnType::getPrimitive(castTy)
                );
            }

        //int -> flt, (flt -> int is never implicit)
        }else if(lIsInt && rIsFlt){
            return TypedValue(
                isUnsignedTypeTag(num.type->typeTag)
                    ? builder.CreateUIToFP(num.val, ty)
                    : builder.CreateSIToFP(num.val, ty),

                AnType::getPrimitive(castTy)
            );

        //float widening
        }else if(lIsFlt && rIsFlt){
            if(lbw < rbw){
                return TypedValue(
                    builder.CreateFPExt(num.val, ty),
                    AnType::getPrimitive(castTy)
                );
            }
        }
    }

    return num;
}


/*
 *  Assures two IntegerType'd Values have the same bitwidth.
 *  If not, one is extended to the larger bitwidth and mutated appropriately.
 *  If the extended integer value is unsigned, it is zero extended, otherwise
 *  it is sign extended.
 *  Assumes the llvm::Type of both values to be an instance of IntegerType.
 */
void Compiler::implicitlyCastIntToInt(TypedValue *lhs, TypedValue *rhs){
    int lbw = getBitWidthOfTypeTag(lhs->type->typeTag);
    int rbw = getBitWidthOfTypeTag(rhs->type->typeTag);

    if(lbw != rbw){
        //Cast the value with the smaller bitwidth to the type with the larger bitwidth
        if(lbw < rbw){
            auto ret = TypedValue(
                builder.CreateIntCast(lhs->val, rhs->getType(),
                    !isUnsignedTypeTag(lhs->type->typeTag)),
                rhs->type
            );

            *lhs = ret;

        }else{//lbw > rbw
            auto ret = TypedValue(
                builder.CreateIntCast(rhs->val, lhs->getType(),
                    !isUnsignedTypeTag(rhs->type->typeTag)),
                lhs->type
            );

            *rhs = ret;
        }
    }
}

bool isIntTypeTag(const TypeTag ty){
    return ty==TT_I8 or ty==TT_I16 or ty==TT_I32 or ty==TT_I64 or
           ty==TT_U8 or ty==TT_U16 or ty==TT_U32 or ty==TT_U64 or
           ty==TT_Isz or ty==TT_Usz or ty==TT_C8;
}

bool isFPTypeTag(const TypeTag tt){
    return tt==TT_F16 or tt==TT_F32 or tt==TT_F64;
}

bool isNumericTypeTag(const TypeTag ty){
    return isIntTypeTag(ty) or isFPTypeTag(ty);
}

/*
 *  Performs an implicit cast from a float to int.  Called in any operation
 *  involving an integer, a float, and a binop.  No matter the ints size,
 *  it is always casted to the (possibly smaller) float value.
 */
void Compiler::implicitlyCastIntToFlt(TypedValue *lhs, Type *ty){
    auto ret = TypedValue(
        isUnsignedTypeTag(lhs->type->typeTag)
            ? builder.CreateUIToFP(lhs->val, ty)
            : builder.CreateSIToFP(lhs->val, ty),

        AnType::getPrimitive(llvmTypeToTypeTag(ty))
    );
    *lhs = ret;
}


/*
 *  Performs an implicit cast from a float to float.
 */
void Compiler::implicitlyCastFltToFlt(TypedValue *lhs, TypedValue *rhs){
    int lbw = getBitWidthOfTypeTag(lhs->type->typeTag);
    int rbw = getBitWidthOfTypeTag(rhs->type->typeTag);

    if(lbw != rbw){
        if(lbw < rbw){
            auto ret = TypedValue(
                builder.CreateFPExt(lhs->val, rhs->getType()),
                rhs->type
            );
            *lhs = ret;
        }else{//lbw > rbw
            auto ret = TypedValue(
                builder.CreateFPExt(rhs->val, lhs->getType()),
                lhs->type
            );
            *rhs = ret;
        }
    }
}


/*
 *  Detects, and creates an implicit type conversion when necessary.
 */
void Compiler::handleImplicitConversion(TypedValue *lhs, TypedValue *rhs){
    bool lIsInt = isIntTypeTag(lhs->type->typeTag);
    bool lIsFlt = isFPTypeTag(lhs->type->typeTag);
    if(!lIsInt && !lIsFlt) return;

    bool rIsInt = isIntTypeTag(rhs->type->typeTag);
    bool rIsFlt = isFPTypeTag(rhs->type->typeTag);
    if(!rIsInt && !rIsFlt) return;

    //both values are numeric, so forward them to the relevant casting method
    if(lIsInt && rIsInt){
        implicitlyCastIntToInt(lhs, rhs);  //implicit int -> int (widening)
    }else if(lIsInt && rIsFlt){
        implicitlyCastIntToFlt(lhs, rhs->getType()); //implicit int -> flt
    }else if(lIsFlt && rIsInt){
        implicitlyCastIntToFlt(rhs, lhs->getType()); //implicit int -> flt
    }else if(lIsFlt && rIsFlt){
        implicitlyCastFltToFlt(lhs, rhs); //implicit int -> flt
    }
}


bool containsTypeVar(const TypeNode *tn){
    auto tt = tn->type;
    if(tt == TT_Array or tt == TT_Ptr){
        return tn->extTy->type == tt;
    }else if(tt == TT_Tuple or tt == TT_Data or tt == TT_TaggedUnion or
             tt == TT_Function or tt == TT_MetaFunction){
        TypeNode *ext = tn->extTy.get();
        while(ext){
            if(containsTypeVar(ext))
                return true;
        }
    }
    return tt == TT_TypeVar;
}


/*
 *  Translates an individual TypeTag to an llvm::Type.
 *  Only intended for primitive types, as there is not enough
 *  information stored in a TypeTag to convert to array, tuple,
 *  or function types.
 */
Type* typeTagToLlvmType(TypeTag ty, LLVMContext &ctxt){
    switch(ty){
        case TT_I8:  case TT_U8:  return Type::getInt8Ty(ctxt);
        case TT_I16: case TT_U16: return Type::getInt16Ty(ctxt);
        case TT_I32: case TT_U32: return Type::getInt32Ty(ctxt);
        case TT_I64: case TT_U64: return Type::getInt64Ty(ctxt);
        case TT_Isz:    return Type::getIntNTy(ctxt, AN_USZ_SIZE); //TODO: implement
        case TT_Usz:    return Type::getIntNTy(ctxt, AN_USZ_SIZE); //TODO: implement
        case TT_F16:    return Type::getHalfTy(ctxt);
        case TT_F32:    return Type::getFloatTy(ctxt);
        case TT_F64:    return Type::getDoubleTy(ctxt);
        case TT_C8:     return Type::getInt8Ty(ctxt);
        case TT_C32:    return Type::getInt32Ty(ctxt);
        case TT_Bool:   return Type::getInt1Ty(ctxt);
        case TT_Void:   return Type::getVoidTy(ctxt);
        case TT_TypeVar:
            throw new TypeVarError();
        default:
            cerr << "typeTagToLlvmType: Unknown/Unsupported TypeTag " << ty << ", returning nullptr.\n";
            return nullptr;
    }
}

AnType* getLargestExt(Compiler *c, AnDataType *tn, bool force){
    AnType *largest = 0;
    size_t largest_size = 0;

    for(auto *e : tn->extTys){
        auto size = e->getSizeInBits(c, nullptr, force);
        if(!size){
            cerr << size.getErr() << endl;
            size = 0;
        }

        if(size.getVal() > largest_size){
            largest = e;
            largest_size = size.getVal();
        }
    }
    return largest;
}



/*
 *  Translates a llvm::Type to a TypeTag. Not intended for in-depth analysis
 *  as it loses data about the type and name of UserTypes, and cannot distinguish
 *  between signed and unsigned integer types.  As such, this should mainly be
 *  used for comparing primitive datatypes, or just to detect if something is a
 *  primitive.
 */
TypeTag llvmTypeToTypeTag(Type *t){
    if(t->isIntegerTy(1)) return TT_Bool;

    if(t->isIntegerTy(8)) return TT_I8;
    if(t->isIntegerTy(16)) return TT_I16;
    if(t->isIntegerTy(32)) return TT_I32;
    if(t->isIntegerTy(64)) return TT_I64;
    if(t->isHalfTy()) return TT_F16;
    if(t->isFloatTy()) return TT_F32;
    if(t->isDoubleTy()) return TT_F64;

    if(t->isArrayTy()) return TT_Array;
    if(t->isStructTy() && !t->isEmptyTy()) return TT_Tuple; /* Could also be a TT_Data! */
    if(t->isPointerTy()) return TT_Ptr;
    if(t->isFunctionTy()) return TT_Function;

    return TT_Void;
}

/*
 *  Converts a TypeNode to an llvm::Type.  While much less information is lost than
 *  llvmTypeToTokType, information on signedness of integers is still lost, causing the
 *  unfortunate necessity for the use of a TypedValue for the storage of this information.
 */
Type* Compiler::anTypeToLlvmType(const AnType *ty, bool force){
    vector<Type*> tys;

    switch(ty->typeTag){
        case TT_Ptr: {
            auto *ptr = try_cast<AnPtrType>(ty);
            return ptr->extTy->typeTag != TT_Void ?
                anTypeToLlvmType(ptr->extTy, force)->getPointerTo()
                : Type::getInt8Ty(*ctxt)->getPointerTo();
        }
        case TT_Type:
            return Type::getInt8Ty(*ctxt)->getPointerTo();
        case TT_Array:{
            auto *arr = try_cast<AnArrayType>(ty);
            return ArrayType::get(anTypeToLlvmType(arr->extTy, force), arr->len);
        }
        case TT_Tuple:
            for(auto *e : try_cast<AnAggregateType>(ty)->extTys){
                auto *ty = anTypeToLlvmType(e, force);
                if(!ty->isVoidTy())
                    tys.push_back(ty);
            }
            return StructType::get(*ctxt, tys);
        case TT_Data: case TT_TaggedUnion: {
            //TODO: remove const from function
            //updatellvmtypebinding breaks const correctness
            auto *dt = (AnDataType*)try_cast<AnDataType>(ty);
            if(dt->isStub()){
                return updateLlvmTypeBinding(this, dt, force);
                //compErr("Use of undeclared type " + dt->name);
            }

            if(dt->llvmType)
                return dt->llvmType;
            else
                return updateLlvmTypeBinding(this, dt, force);
        }
        case TT_Function: case TT_MetaFunction: {
            auto *f = try_cast<AnAggregateType>(ty);
            for(size_t i = 1; i < f->extTys.size(); i++){
                tys.push_back(anTypeToLlvmType(f->extTys[i], force));
            }

            return FunctionType::get(anTypeToLlvmType(f->extTys[0], force), tys, false)->getPointerTo();
        }
        case TT_TypeVar: {
            auto *tvt = try_cast<AnTypeVarType>(ty);
            AnType *binding = lookupTypeVar(tvt->name);
            if(!binding){
                //compErr("Use of undeclared type variable " + ty->typeName, ty->loc);
                //compErr("tn2llvmt: TypeVarError; lookup for "+ty->typeName+" not found", ty->loc);
                //throw new TypeVarError();
                if(!force)
                    cerr << "Warning: cannot translate undeclared typevar " << tvt->name << endl;

                return Type::getInt64PtrTy(*ctxt);
            }

            if(binding == tvt){
                cerr << "Warning: typevar " << tvt->name << " refers to itself" << endl;
                return Type::getVoidTy(*ctxt);
            }

            return anTypeToLlvmType(binding, force);
        }
        default:
            return typeTagToLlvmType(ty->typeTag, *ctxt);
    }
}


/*
 *  Returns true if two given types are approximately equal.  This will return
 *  true if they are the same primitive datatype, or are both pointers pointing
 *  to the same elementtype, or are both arrays of the same element type, even
 *  if the arrays differ in size.  If two types are needed to be exactly equal,
 *  pointer comparison can be used instead since llvm::Types are uniqued.
 */
bool llvmTypeEq(Type *l, Type *r){
    TypeTag ltt = llvmTypeToTypeTag(l);
    TypeTag rtt = llvmTypeToTypeTag(r);

    if(ltt != rtt) return false;

    if(ltt == TT_Ptr){
        Type *lty = l->getPointerElementType();
        Type *rty = r->getPointerElementType();

        if(lty->isVoidTy() or rty->isVoidTy()) return true;

        return llvmTypeEq(lty, rty);
    }else if(ltt == TT_Array){
        return l->getArrayElementType() == r->getArrayElementType() and
               l->getArrayNumElements() == r->getArrayNumElements();
    }else if(ltt == TT_Function or ltt == TT_MetaFunction){
        int lParamCount = l->getFunctionNumParams();
        int rParamCount = r->getFunctionNumParams();

        if(lParamCount != rParamCount)
            return false;

        for(int i = 0; i < lParamCount; i++){
            if(!llvmTypeEq(l->getFunctionParamType(i), r->getFunctionParamType(i)))
                return false;
        }
        return true;
    }else if(ltt == TT_Tuple or ltt == TT_Data){
        int lElemCount = l->getStructNumElements();
        int rElemCount = r->getStructNumElements();

        if(lElemCount != rElemCount)
            return false;

        for(int i = 0; i < lElemCount; i++){
            if(!llvmTypeEq(l->getStructElementType(i), r->getStructElementType(i)))
                return false;
        }

        return true;
    }else{ //primitive type
        return true; /* true since ltt != rtt check above is false */
    }
}


TypeCheckResult& TypeCheckResult::success(size_t matches){
    if(box->res != Failure){
        box->matches += matches;
    }
    return *this;
}


TypeCheckResult& TypeCheckResult::success(){
    if(box->res != Failure){
        box->matches++;
    }
    return *this;
}

TypeCheckResult& TypeCheckResult::successWithTypeVars(){
    if(box->res != Failure){
        box->res = SuccessWithTypeVars;
    }
    return *this;
}

TypeCheckResult& TypeCheckResult::failure(){
    box->res = Failure;
    return *this;
}

TypeCheckResult& TypeCheckResult::successIf(bool b){
    if(b) return success();
    else  return failure();
}

TypeCheckResult& TypeCheckResult::successIf(Result r){
    if(r == Success)
        return success();
    else if(r == SuccessWithTypeVars)
        return successWithTypeVars();
    else
        return failure();
}

bool TypeCheckResult::failed(){
    return box->res == Failure;
}


//forward decl of typeEqHelper for extTysEq fn
TypeCheckResult& typeEqHelper(const Compiler *c, const AnType *l, const AnType *r, TypeCheckResult &tcr);

/*
 *  Helper function to check if each type's list of extension
 *  types are all approximately equal.  Used when checking the
 *  equality of AnTypes of type Tuple, Data, Function, or any
 *  type with multiple extTys.
 */
TypeCheckResult& extTysEq(const AnAggregateType *lAgg, const AnAggregateType *rAgg, TypeCheckResult &tcr, const Compiler *c = 0){
    if(lAgg->extTys.size() != rAgg->extTys.size())
        return tcr.failure();

    for(size_t i = 0; i < lAgg->extTys.size(); i++){
        auto *lExt = lAgg->extTys[i];
        auto *rExt = rAgg->extTys[i];

        if(c){
            if(!typeEqHelper(c, lExt, rExt, tcr)) return tcr.failure();
        }else{
            if(!typeEqBase(lExt, rExt, tcr)) return tcr.failure();
        }
    }
    return tcr.success();
}

/*
 *  Returns 1 if two types are approx eq
 *  Returns 2 if two types are approx eq and one is a typevar
 *
 *  Does not check for trait implementation unless c is set.
 *
 *  This function is used as a base for typeEq, if a typeEq function
 *  is needed that does not require a Compiler parameter, this can be
 *  used, although it does not check for trait impls.  The optional
 *  Compiler parameter here is only used by the typeEq function.  If
 *  this function is used as a typeEq function with the Compiler ptr
 *  the outermost type will not be checked for traits.
 */
TypeCheckResult& typeEqBase(const AnType *l, const AnType *r, TypeCheckResult &tcr, const Compiler *c){
    if(l == r && !l->isGeneric) return tcr.success(l->numMatchedTys);

    if(l->typeTag == TT_TaggedUnion && r->typeTag == TT_Data)
        return tcr.successIf(try_cast<AnDataType>(l)->name == try_cast<AnDataType>(r)->name);

    if(l->typeTag == TT_Data && r->typeTag == TT_TaggedUnion)
        return tcr.successIf(try_cast<AnDataType>(l)->name == try_cast<AnDataType>(r)->name);

    //typevars should be handled by typeEqHelper which requires the Compiler param,
    //if typeEqBase is called without a Compiler param they will return success without any bindings
    if(l->typeTag == TT_TypeVar or r->typeTag == TT_TypeVar)
        return tcr.successWithTypeVars();


    if(l->typeTag != r->typeTag)
        return tcr.failure();

    if(auto *lptr = try_cast<AnPtrType>(l)){
        auto *rptr = try_cast<AnPtrType>(r);
        tcr->matches++;

        return c ? typeEqHelper(c, lptr->extTy, rptr->extTy, tcr)
                 : typeEqBase(lptr->extTy, rptr->extTy, tcr, c);

    }else if(auto *larr = try_cast<AnArrayType>(l)){
        auto *rarr = try_cast<AnArrayType>(r);
        tcr->matches++;

        if(larr->len != rarr->len) return tcr.failure();

        return c ? typeEqHelper(c, larr->extTy, rarr->extTy, tcr)
                 : typeEqBase(larr->extTy, rarr->extTy, tcr, c);

    /* This case should be handled by typeEqHelper and should only reach here if there is no
     * valid Compiler* parameter to lookup definitions from. */
    }else if(auto *ldt = try_cast<AnDataType>(l)){
        auto *rdt = try_cast<AnDataType>(r);
        return tcr.successIf(ldt->name == rdt->name);

    }else if(auto *lAgg = try_cast<AnAggregateType>(l)){
        return extTysEq(lAgg, try_cast<AnAggregateType>(r), tcr, c);
    }
    //primitive type, we already know l->type == r->type
    return tcr.success();
}

bool dataTypeImplementsTrait(AnDataType *dt, string trait){
    for(auto traitImpl : dt->traitImpls){
        if(traitImpl->name == trait)
            return true;
    }
    return false;
}


TypeCheckResult& typeCheckBoundDataTypes(const Compiler *c, const AnDataType *l,
        const AnDataType *r, TypeCheckResult &tcr){

    if(l->boundGenerics.size() != r->boundGenerics.size())
        return tcr.failure();

    for(size_t i = 0; i < l->boundGenerics.size(); i++){
        auto &lbg = l->boundGenerics[i];
        auto &rbg = r->boundGenerics[i];

        if(!typeEqHelper(c, lbg.getBinding(), rbg.getBinding(), tcr)) return tcr.failure();
    }
    return tcr.success();
}


TypeCheckResult& addBindingsToTypeCheck(const Compiler *c, vector<TypeBinding> const& bindings, TypeCheckResult &tcr){
    for(auto &b : bindings){
        auto *repeat = findBindingFor(b.getGenericTypeParam(), tcr->bindings);
        if(repeat){
            auto tc2 = TypeCheckResult();
            typeEqHelper(c, b.getBinding(), repeat->getBinding(), tc2);
            if(!tc2) return tcr.failure();
        }else{
            tcr->bindings.push_back(b);
        }
    }
    tcr->matches++;
    return tcr.successWithTypeVars();
}


/**
 * Returns the type check result of two possibly generic AnDataTypes with matching typenames.
 */
TypeCheckResult& typeCheckVariants(const Compiler *c, const AnDataType *l,
        const AnDataType *r, TypeCheckResult &tcr){

    bool lIsBound = l->isVariant();
    bool rIsBound = r->isVariant();

    //Two bound variants are equal if their type parameters are equal
    if(lIsBound && rIsBound){
        for(size_t i = 0; i < l->boundGenerics.size(); i++){
            typeEqHelper(c, l->boundGenerics[i].getBinding(), r->boundGenerics[i].getBinding(), tcr);
            if(tcr.failed()) return tcr;
        }
        return tcr.success();
    //Perform type checks to get the needed bindings of type
    //variables to bind an unbound variant to a given bound variant.
    }else if(lIsBound && !rIsBound){
        return addBindingsToTypeCheck(c, l->boundGenerics, tcr);
    }else if(!lIsBound && rIsBound){
        return addBindingsToTypeCheck(c, r->boundGenerics, tcr);
    //neither are bound, these should both be parent types
    }else{
        return tcr.success();
    }
}


/*
 *  Return true if both typenodes are approximately equal
 *
 *  Compiler instance required to check for trait implementation
 */
TypeCheckResult& typeEqHelper(const Compiler *c, const AnType *l, const AnType *r, TypeCheckResult &tcr){
    if(l == r && !l->isGeneric) return tcr.success(l->numMatchedTys);
    if(!r) return tcr.failure();

    //check for type aliases
    const AnDataType *dt;
    if((dt = try_cast<AnDataType>(l)) && dt->isAlias){
        return typeEqHelper(c, dt->getAliasedType(), r, tcr);
    }else if((dt = try_cast<AnDataType>(r)) && dt->isAlias){
        return typeEqHelper(c, l, dt->getAliasedType(), tcr);
    }

    const AnDataType *ldt, *rdt;
    if((ldt = try_cast<AnDataType>(l)) && (rdt = try_cast<AnDataType>(r))){
        if(ldt->name == rdt->name && !ldt->isVariant() && !rdt->isVariant())
            return tcr.success();

        if(ldt->unboundType && ldt->unboundType == rdt->unboundType){
            return typeCheckBoundDataTypes(c, ldt, rdt, tcr);
        }

        if(ldt->name == rdt->name){
            return typeCheckVariants(c, ldt, rdt, tcr);
        }

        //typeName's are different, check if one is a trait and the other
        //is an implementor of the trait
        Trait *t;
        AnDataType *dt;
        if((t = c->lookupTrait(ldt->name))){
            dt = AnDataType::get(rdt->name);
            if(!dt or dt->isStub()) return tcr.failure();
        }else if((t = c->lookupTrait(rdt->name))){
            dt = AnDataType::get(ldt->name);
            if(!dt or dt->isStub()) return tcr.failure();
        }else{
            return tcr.failure();
        }

        return tcr.successIf(dataTypeImplementsTrait(dt, t->name));

    }else if(l->typeTag == TT_TypeVar or r->typeTag == TT_TypeVar){

        //reassign l and r into typeVar and nonTypeVar so code does not have to be repeated in
        //one if branch for l and another for r
        const AnTypeVarType *typeVar;
        AnType *nonTypeVar;

        if(l->typeTag == TT_TypeVar && r->typeTag != TT_TypeVar){
            typeVar = try_cast<AnTypeVarType>(l);
            //cast away const to store in string,AnType* pair later
            nonTypeVar = (AnType*)r;
        }else if(l->typeTag != TT_TypeVar && r->typeTag == TT_TypeVar){
            typeVar = try_cast<AnTypeVarType>(r);
            nonTypeVar = (AnType*)l;
        }else{ //both type vars
            auto *ltv = try_cast<AnTypeVarType>(l);
            auto *rtv = try_cast<AnTypeVarType>(r);

            //lookup the type bound to ltv
            AnType *lv = c->lookupTypeVar(ltv->name);

            //If they are equal, return before doing the second lookup
            if(ltv == rtv){
                if(lv){
                    tcr->bindings.emplace_back(ltv->name, lv);
                    return tcr.successWithTypeVars();
                }else{
                    //Binding for the equal typevars not found in scope,
                    //so dont add it to bindings, just return Success
                    //since 't == 't even if 't is unbound
                    return tcr.success();
                }
            }

            AnType *rv = c->lookupTypeVar(rtv->name);

            if(lv && rv){ //both are already bound
                //add bindings from scope to the TypeCheckResult
                //and recur on them to make sure they're equal
                tcr->bindings.emplace_back(ltv->name, lv);
                tcr->bindings.emplace_back(rtv->name, rv);
                tcr.successWithTypeVars();
                return typeEqHelper(c, lv, rv, tcr);
            }else if(lv && not rv){
                typeVar = rtv; //rtv binding not found so it stays as a typevar
                nonTypeVar = lv;
                tcr->bindings.emplace_back(ltv->name, nonTypeVar);
                //fall through to successWithTypeVars below
            }else if(rv && not lv){
                typeVar = ltv;
                nonTypeVar = rv;
                tcr->bindings.emplace_back(rtv->name, nonTypeVar);
                //fall through to successWithTypeVars below
            }else{ //neither are bound
                return tcr.success();
            }
        }

        auto *tv = findBindingFor(typeVar->name, tcr->bindings);
        if(!tv){
            tcr->bindings.emplace_back(typeVar->name, nonTypeVar);

            return tcr.successWithTypeVars();
        }else{
            //tv is bound in same typechecking run
            //Create fake TypeCheckResult to avoid adding
            //the matches from the typevar's bound value to the concrete param.
            //This ensures ('t, 't) has 1 match with (i32, i32), the tuple's structure.
            //Not 2: the tuple structure and the second 't that is already bound to i32
            auto tc2 = TypeCheckResult();
            typeEqHelper(c, tv->getBinding(), nonTypeVar, tc2);
            if(!tc2) return tcr.failure();

            if(tc2->res == TypeCheckResult::SuccessWithTypeVars){
                tcr->res = TypeCheckResult::SuccessWithTypeVars;
                for(auto &b : tc2->bindings)
                    tcr->bindings.push_back(b);
            }
            return tcr;
        }
    }
    return typeEqBase(l, r, tcr, c);
}

TypeCheckResult Compiler::typeEq(const AnType *l, const AnType *r) const{
    auto tcr = TypeCheckResult();
    typeEqHelper(this, l, r, tcr);
    return tcr;
}


TypeCheckResult Compiler::typeEq(vector<AnType*> l, vector<AnType*> r) const{
    auto tcr = TypeCheckResult();
    if(l.size() != r.size()){
        tcr.failure();
        return tcr;
    }

    for(size_t i = 0; i < l.size(); i++){
        typeEqHelper(this, l[i], r[i], tcr);
        if(tcr.failed()) return tcr;
    }
    return tcr;
}


/*
 *  Returns true if the given typetag is a primitive type, and thus
 *  accurately represents the entire type without information loss.
 *  NOTE: this function relies on the fact all primitive types are
 *        declared before non-primitive types in the TypeTag definition.
 */
bool isPrimitiveTypeTag(TypeTag ty){
    return ty >= TT_I8 && ty <= TT_Bool;
}


/*
 *  Converts a TypeTag to its string equivalent for
 *  helpful error messages.  For most cases, llvmTypeToStr
 *  should be used instead to provide the full type.
 */
string typeTagToStr(TypeTag ty){

    switch(ty){
        case TT_I8:    return "i8" ;
        case TT_I16:   return "i16";
        case TT_I32:   return "i32";
        case TT_I64:   return "i64";
        case TT_U8:    return "u8" ;
        case TT_U16:   return "u16";
        case TT_U32:   return "u32";
        case TT_U64:   return "u64";
        case TT_F16:   return "f16";
        case TT_F32:   return "f32";
        case TT_F64:   return "f64";
        case TT_Isz:   return "isz";
        case TT_Usz:   return "usz";
        case TT_C8:    return "c8" ;
        case TT_C32:   return "c32";
        case TT_Bool:  return "bool";
        case TT_Void:  return "void";
        case TT_Type:  return "Type";

        /*
         * Because of the loss of specificity for these last types,
         * these strings are most likely insufficient.  The entire
         * AnType should be preferred to be used instead.
         */
        case TT_Tuple:        return "Tuple";
        case TT_Array:        return "Array";
        case TT_Ptr:          return "Ptr"  ;
        case TT_Data:         return "Data" ;
        case TT_TypeVar:      return "'t";
        case TT_Function:     return "Function";
        case TT_MetaFunction: return "Meta Function";
        case TT_FunctionList: return "Function List";
        case TT_TaggedUnion:  return "|";
        default:              return "(Unknown TypeTag " + to_string(ty) + ")";
    }
}

/*
 *  Converts a typeNode directly to a string with no information loss.
 *  Used in ExtNode::compile
 */
string typeNodeToStr(const TypeNode *t){
    if(!t) return "null";

    if(t->type == TT_Tuple){
        string ret = "(";
        TypeNode *elem = t->extTy.get();
        while(elem){
            if(elem->next.get())
                ret += typeNodeToStr(elem) + ", ";
            else
                ret += typeNodeToStr(elem) + ")";
            elem = (TypeNode*)elem->next.get();
        }
        return ret;
    }else if(t->type == TT_Data or t->type == TT_TaggedUnion or t->type == TT_TypeVar){
        string name = t->typeName;
        if(!t->params.empty()){
            name += "<";
            name += typeNodeToStr(t->params[0].get());
            for(unsigned i = 1; i < t->params.size(); i++){
                name += ", ";
                name += typeNodeToStr(t->params[i].get());
            }
            name += ">";
        }
        return name;
    }else if(t->type == TT_Array){
        auto *len = (IntLitNode*)t->extTy->next.get();
        return '[' + len->val + " " + typeNodeToStr(t->extTy.get()) + ']';
    }else if(t->type == TT_Ptr){
        return typeNodeToStr(t->extTy.get()) + "*";
    }else if(t->type == TT_Function or t->type == TT_MetaFunction){
        string ret = "(";
        string retTy = typeNodeToStr(t->extTy.get());
        TypeNode *cur = (TypeNode*)t->extTy->next.get();
        while(cur){
            ret += typeNodeToStr(cur);
            cur = (TypeNode*)cur->next.get();
            if(cur) ret += ",";
        }
        return ret + ")->" + retTy;
    }else{
        return typeTagToStr(t->type);
    }
}


string anTypeToStr(const AnType *t){
    if(!t) return "(null)";

    /** Must check for modifiers first as they can be lost after dyn_cast */
    if(t->isModifierType()){
        if(auto *mod = dynamic_cast<const BasicModifier*>(t)){
            return Lexer::getTokStr(mod->mod) + ' ' + anTypeToStr(mod->extTy);

        }else if(auto *cdmod = dynamic_cast<const CompilerDirectiveModifier*>(t)){
            //TODO: modify printingvisitor to print to streams
            // PrintingVisitor::print(cdmod->directive.get());
            return anTypeToStr(cdmod->extTy);
        }else{
            return "(unknown modifier type)";
        }
    }else if(auto *dt = try_cast<AnDataType>(t)){
        string n = dt->name;

        if(dt->isVariant()){
            n += "<";
            for(auto &t : dt->boundGenerics){
                if(&t == &dt->boundGenerics.back()){
                    n += anTypeToStr(t.getBinding());
                }else{
                    n += anTypeToStr(t.getBinding()) + ", ";
                }
            }
            n += ">";
        }
        return n;
    }else if(auto *tvt = try_cast<AnTypeVarType>(t)){
        return tvt->name;
    }else if(auto *f = try_cast<AnFunctionType>(t)){
        string ret = "(";
        string retTy = anTypeToStr(f->retTy);
        if(f->extTys.size() == 1){
            return anTypeToStr(f->extTys[0]) + " -> " + retTy;
        }

        for(auto &param : f->extTys){
            ret += anTypeToStr(param);
            if(&param != &f->extTys.back())
                ret += ", ";
        }
        return ret + ") -> " + retTy;
    }else if(auto *tup = try_cast<AnAggregateType>(t)){
        string ret = "(";

        for(const auto &ext : tup->extTys){
            ret += anTypeToStr(ext);
            if(&ext != &tup->extTys.back())
                ret += ", ";
        }
        return ret + ")";
    }else if(auto *arr = try_cast<AnArrayType>(t)){
        return '[' + to_string(arr->len) + " " + anTypeToStr(arr->extTy) + ']';
    }else if(auto *ptr = try_cast<AnPtrType>(t)){
        return anTypeToStr(ptr->extTy) + "*";
    }else{
        return typeTagToStr(t->typeTag);
    }
}


/*
 *  Returns a string representing the full type of ty.  Since it is converting
 *  from a llvm::Type, this will never return an unsigned integer type.
 */
string llvmTypeToStr(Type *ty){
    if(!ty) return "(null)";

    TypeTag tt = llvmTypeToTypeTag(ty);
    if(isPrimitiveTypeTag(tt)){
        return typeTagToStr(tt);
    }else if(tt == TT_Tuple){
        if(!ty->getStructName().empty())
            return string(ty->getStructName());

        string ret = "(";
        const unsigned size = ty->getStructNumElements();

        for(unsigned i = 0; i < size; i++){
            if(i == size-1){
                ret += llvmTypeToStr(ty->getStructElementType(i)) + ")";
            }else{
                ret += llvmTypeToStr(ty->getStructElementType(i)) + ", ";
            }
        }
        return ret;
    }else if(tt == TT_Array){
        return "[" + to_string(ty->getArrayNumElements()) + " " + llvmTypeToStr(ty->getArrayElementType()) + "]";
    }else if(tt == TT_Ptr){
        return llvmTypeToStr(ty->getPointerElementType()) + "*";
    }else if(tt == TT_Function){
        string ret = "func("; //TODO: get function return type
        const unsigned paramCount = ty->getFunctionNumParams();

        for(unsigned i = 0; i < paramCount; i++){
            if(i == paramCount-1)
                ret += llvmTypeToStr(ty->getFunctionParamType(i)) + ")";
            else
                ret += llvmTypeToStr(ty->getFunctionParamType(i)) + ", ";
        }
        return ret;
    }else if(tt == TT_TypeVar){
        return "(typevar)";
    }else if(tt == TT_Void){
        return "void";
    }
    return "(Unknown type)";
}

} //end of namespace ante
