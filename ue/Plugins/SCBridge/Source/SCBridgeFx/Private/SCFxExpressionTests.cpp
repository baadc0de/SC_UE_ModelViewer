// SCBridge.Fx.ExpressionValidation: the engine fork's stateless expression validation (WP-E1 E1-6 review), which the emitter build
// runs before it builds a spawn or loop expression (an invalid one becomes the distribution's constant, with a warning).
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "NiagaraTypes.h"
#include "Stateless/NiagaraStatelessExpression.h"
#include "Stateless/Expressions/NiagaraStatelessExpressionFloat.h"
#include "Stateless/Expressions/NiagaraStatelessExpressionVec3.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSCFxExpressionValidationTest, "SCBridge.Fx.ExpressionValidation",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSCFxExpressionValidationTest::RunTest(const FString&)
{
    const FNiagaraTypeDefinition Float = FNiagaraTypeDefinition::GetFloatDef();
    FString Error;
    const auto Bind = [](FName Name) {
        FNiagaraStatelessExpressionFloatBinding B;
        B.A = Name;
        return FInstancedStruct::Make(B);
    };
    const auto Mul = [](FInstancedStruct A, FInstancedStruct B) {
        FNiagaraStatelessExpressionFloatMultiply M;
        M.A = MoveTemp(A);
        M.B = MoveTemp(B);
        return FInstancedStruct::Make(M);
    };
    FNiagaraStatelessExpressionFloatConstant Two;
    Two.A = 2.f;
    TestTrue(TEXT("a bound rate x 2 is valid"), FNiagaraStatelessExpression::ValidateTree(Mul(Bind(TEXT("User.SC.BeamLength")), FInstancedStruct::Make(Two)), Float, Error));
    TestFalse(TEXT("an empty root is not"), FNiagaraStatelessExpression::ValidateTree(FInstancedStruct(), Float, Error));
    TestFalse(TEXT("a binding without a name (NAME_None leaf: offset -4) is not"), FNiagaraStatelessExpression::ValidateTree(Mul(Bind(NAME_None), FInstancedStruct::Make(Two)), Float, Error));
    TestTrue(TEXT("the error names the leaf"), Error.Contains(TEXT("binds no parameter")));
    TestFalse(TEXT("an empty operand is not"), FNiagaraStatelessExpression::ValidateTree(Mul(FInstancedStruct(), FInstancedStruct::Make(Two)), Float, Error));
    FNiagaraStatelessExpressionVec3Constant Vec;
    TestFalse(TEXT("a vec3 root for a float distribution is not"), FNiagaraStatelessExpression::ValidateTree(FInstancedStruct::Make(Vec), Float, Error));
#if WITH_EDITORONLY_DATA
    TestFalse(TEXT("a vec3 operand of a float multiply is not (BaseStruct)"), FNiagaraStatelessExpression::ValidateTree(Mul(FInstancedStruct::Make(Vec), FInstancedStruct::Make(Two)), Float, Error));
#endif
    return true;
}
#endif
