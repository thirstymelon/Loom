#!/usr/bin/env python3
import pathlib
import subprocess
import sys
import tempfile

compiler = pathlib.Path(sys.argv[1])
build = pathlib.Path(sys.argv[2])
canonical = pathlib.Path(__file__).resolve().parent / "fixtures" / "basic.loom"

positive = {
    "canonical_named_ends": (canonical.read_text(), ["define i32 @Add", "define void @Main", "ret i32"]),
    "procedure_natural_end": ("Block Main\nImplements\nEnd Main\n", ["define void @Main", "ret void"]),
    "infinite_loop_no_fallthrough": ("Block Spin\nImplements\n Loop\n End Loop\nEnd Spin\n", ["loop.header"]),
    "constant_true_give_coverage": ("Block F Produces Signed_32\nImplements\n If True\n  Give 3\n End If\nEnd F\n", ["define i32 @F", "ret i32"]),
    "constant_false_if": ("Block Main Declares\n X : Signed_32\nImplements\n If False\n  X = 1\n End If\nEnd Main\n", ["define void @Main", "ret void"]),
    "give_from_if_then": ("Block F Takes\n B : Boolean\nProduces Signed_32\nImplements\n If B\n  Give 3\n End If\n Give 4\nEnd F\n", ["if.then", "ret i32"]),
    "multiline_takes_two_signed32": ("Block Add Takes\n A : Signed_32\n B : Signed_32\nProduces Signed_32\nImplements\n Give A + B\nEnd Add\n", ["define i32 @Add(i32 %A, i32 %B)", "add i32"]),
    "precedence_parentheses": ("Block Calc Takes\n X : Signed_32\n Y : Signed_32\nProduces Signed_32\nImplements\n Give (X + 3) * 4 - 8 / Y\nEnd Calc\n", ["add i32", "mul i32", "sdiv i32", "sub i32"]),
    "left_associative_subtraction": ("Block F Takes\n A : Signed_32\n B : Signed_32\n C : Signed_32\nProduces Signed_32\nImplements\n Give A - B - C\nEnd F\n", ["sub i32"]),
    "boolean_and_not": ("Block Test Takes\n A : Boolean\n B : Boolean\nProduces Boolean\nImplements\n Give Not A And B Or A\nEnd Test\n", ["and.rhs", "or.rhs", "xor i1"]),
    "boolean_equal": ("Block Test Takes\n A : Boolean\nProduces Boolean\nImplements\n Give A == False\nEnd Test\n", ["icmp eq i1"]),
    "locals_if_compare": ("Block Main Declares\n X : Signed_16\nImplements\n X = 9\n If X < 10\n X = X + 1\n End If\nEnd Main\n", ["alloca i16", "icmp slt i16", "if.end"]),
    "local_expression_initializers": ("Block Main Declares\n A : Signed_32 = 4\n B : Signed_32 = 5\n Total : Signed_32 = Add(A, B)\nImplements\n Write(Total)\nEnd Main\nBlock Add Takes\n A : Signed_32\n B : Signed_32\nProduces Signed_32\nImplements\n Give A + B\nEnd Add\n", ["store i32 4", "store i32 5", "call i32 @Add", "call i32 (ptr, ...) @printf"]),
    "loop_exit_skip": ("Block Main Declares\n B : Boolean\nImplements\n Loop\n  If B\n   Skip\n  End If\n  Exit\n End Loop\nEnd Main\n", ["loop.header", "loop.exit", "br label %loop.header"]),
    "nested_loop_targets": ("Block Main\nImplements\n Loop\n  Loop\n   Exit\n  End Loop\n  Skip\n End Loop\nEnd Main\n", ["loop.header1", "br label %loop.exit2", "br label %loop.header"]),
    "nested_inner_skip": ("Block Main Declares\n B : Boolean\nImplements\n Loop\n  Loop\n   If B\n    Skip\n   End If\n   Exit\n  End Loop\n  Exit\n End Loop\nEnd Main\n", ["br label %loop.header1", "br label %loop.exit2", "br label %loop.exit"]),
    "write_string": ("Block Main\nImplements\n Write(\"Hello\")\n Write(\"\")\nEnd Main\n", ["call i32 (ptr, ...) @printf", ".loom.string.", ".loom.write.character"]),
    "function_call": ("Block Add Takes\n X : Signed_32\nProduces Signed_32\nImplements\n Give X + 1\nEnd Add\nBlock Main Declares\n Y : Signed_32\nImplements\n Y = Add(41)\nEnd Main\n", ["call i32 @Add(i32 41)"]),
    "forward_and_recursive_call": ("Block Main Declares\n Y : Signed_32\nImplements\n Y = SumTo(4)\nEnd Main\nBlock SumTo Takes\n N : Signed_32\nProduces Signed_32\nImplements\n If N <= 0\n  Give 0\n Else\n  Give N + SumTo(N - 1)\n End If\nEnd SumTo\n", ["call i32 @SumTo", "if.else"]),
    "procedure_call_statement": ("Block Log Implements\n Write(\"logged\")\nEnd Log\nBlock Main Implements\n Log()\nEnd Main\n", ["call void @Log()"]),
    "else_complete_result": ("Block Choose Takes\n B : Boolean\nProduces Signed_32\nImplements\n If B\n  Give 1\n Else\n  Give 2\n End If\nEnd Choose\n", ["if.else", "ret i32 1", "ret i32 2"]),
    "loop_until": ("Block Main Declares\n N : Signed_32\n Ready : Boolean\nImplements\n Loop Until Ready\n  N = N + 1\n  Ready = N >= 2\n End Loop\nEnd Main\n", ["loop.header", "loop.body", "icmp sge i32"]),
    "float_ops": ("Block Twice Takes\n X : Float_64\nProduces Float_64\nImplements\n Give X * 2.5 - 1.0 / 2.0\nEnd Twice\n", ["define double @Twice", "fmul double", "fsub double"]),
    "string_assignment_and_write": ("Block Main Declares\n S : String\nImplements\n S = \"UTF-8: café\\n\"\n Write(S)\nEnd Main\n", [".loom.string.", "call i32 (ptr, ...) @printf"]),
    "write_scalar_and_record_types": ("Record Pair\n Left : Signed_16\n Label : String\nEnd Pair\nEnum Mode : Unsigned_8\n Active = 2\nEnd Mode\nBlock Main Declares\n N : Signed_8 = -3\n U : Unsigned_64 = 9\n F : Float_32 = 1.5\n Ready : Boolean = True\n C : Character = 'é'\n State : Mode = Mode.Active\n P : Pair\nImplements\n P.Left = 4\n P.Label = \"x\"\n Write(N)\n Write(U)\n Write(F)\n Write(Ready)\n Write(C)\n Write(State)\n Write(P)\nEnd Main\n", ["%llu", "%g", "True", "two.byte", "Pair{", "Left: ", "Label: "]),
    "character_literal": ("Block Main Declares\n C : Character\nImplements\n C = 'é'\nEnd Main\n", ["store i32 233"]),
    "module_globals_and_constants": ("Module Example\nBlock Global\nDeclares\n Rate : Constant Unsigned_32 = 1000\n Count : Signed_32 = 4\n Label : Constant String = \"ready\\n\"\nEnd Global\nBlock Main\nImplements\n Count = Count + 1\n Write(Label)\nEnd Main\n", ["@Rate = constant i32 1000", "@Count = global i32 4", "@Label = constant ptr", "store i32"]),
    "enum_declaration_use": ("Enum Mode : Unsigned_8\n Idle = 0\n Flying = 2\nEnd Mode\nBlock Main Declares\n Current : Mode\nImplements\n Current = Mode.Flying\n If Current == Mode.Flying\n  Write(\"flying\")\n End If\nEnd Main\n", ["alloca i8", "store i8 2", "icmp eq i8"]),
    "enum_global_constant": ("Enum Mode : Unsigned_8\n Idle = 0\n Flying = 2\nEnd Mode\nBlock Global\nDeclares\n Default_Mode : Constant Mode = Mode.Idle\nEnd Global\nBlock Main Implements\nEnd Main\n", ["@Default_Mode = constant i8 0"]),
    "record_field_read_write": ("Record Sensor_Data\n Roll : Float_32\n Pitch : Signed_32\nEnd Sensor_Data\nBlock Main Declares\n Sensor : Sensor_Data\n Value : Float_32\nImplements\n Sensor.Roll = 1.5\n Sensor.Pitch = 12\n Value = Sensor.Roll\nEnd Main\n", ["%Sensor_Data = type { float, i32 }", "getelementptr inbounds nuw %Sensor_Data", "store float 1.500000e+00"]),
    "nested_record_field_read": ("Record Point\n X : Signed_32\nEnd Point\nRecord Sensor\n Position : Point\nEnd Sensor\nBlock Main Declares\n Device : Sensor\n X : Signed_32\nImplements\n X = Device.Position.X\nEnd Main\n", ["%Point = type { i32 }", "%Sensor = type { %Point }", "getelementptr inbounds nuw %Sensor"]),
    "global_record_zero_init": ("Record Pair\n Left : Signed_32\n Right : Signed_32\nEnd Pair\nBlock Global\nDeclares\n Shared : Pair\nEnd Global\nBlock Main Implements\n Shared.Left = 5\nEnd Main\n", ["@Shared = global %Pair zeroinitializer", "store i32 5, ptr @Shared"]),
    "constant_prefix_global_and_write_uart": ("Module UartTest\nBlock Global\nDeclares\n BAUD : Constant Signed_32 = 115200\n PIN : Constant Signed_32 = 25\n State : Signed_32 = 1\nEnd Global\nBlock Main\nDeclares\n Msg : String = \"RP2040\"\n Count : Signed_32 = 10\n Local_Const : Constant Signed_32 = 999\nImplements\n Write_UART(\"Baud: <BAUD>, Pin: <PIN>, Local: <Local_Const>\")\n Write_UART(Msg)\n Write_UART(123)\n Write_UART(\"Count: <Count>\")\nEnd Main\n", ["@BAUD = constant i32 115200", "@PIN = constant i32 25", "@State = global i32 1", "call i32 (ptr, ...) @printf"]),
    "give_inside_loop": ("Block Value Produces Signed_8\nImplements\n Loop\n  Give 7\n End Loop\nEnd Value\n", ["define i8 @Value", "ret i8"]),
    "pointer_address_and_deref": ("Block Main Declares\n X : Signed_32 = 10\n P : Pointer[Signed_32]\nImplements\n P = Address(X)\n Deref(P) = 42\n Write(X)\nEnd Main\n", ["store i32 42", "call i32 (ptr, ...) @printf"]),
    "pointer_record_fields": ("Record Point\n X : Signed_32\n Y : Signed_32\nEnd Point\nBlock Main Declares\n Pt : Point\n Ptr : Pointer[Point]\nImplements\n Ptr = Address(Pt)\n Deref(Ptr).X = 100\n Deref(Ptr).Y = 200\n Write(Pt)\nEnd Main\n", ["getelementptr inbounds nuw %Point", "Point{"]),
    "block_gives_record": ("Record Point\n X : Signed_32\n Y : Signed_32\nEnd Point\nBlock MakePoint Takes\n X : Signed_32\n Y : Signed_32\nProduces Point\nDeclares\n P : Point\nImplements\n P.X = X\n P.Y = Y\n Give P\nEnd MakePoint\nBlock Main Declares\n Pt : Point\nImplements\n Pt = MakePoint(1, 2)\n Write(Pt)\nEnd Main\n", ["define %Point @MakePoint", "call %Point @MakePoint", "ret %Point"]),
}
for bits in (8, 16, 32, 64):
    positive[f"signed_width_{bits}"] = (f"Block F Produces Signed_{bits}\nImplements\n Give 1\nEnd F\n", [f"define i{bits} @F"])
    positive[f"unsigned_width_{bits}"] = (f"Block F Produces Unsigned_{bits}\nImplements\n Give 1\nEnd F\n", [f"define i{bits} @F"])
positive["unsigned_ops"] = ("Block F Takes\n A : Unsigned_32\n B : Unsigned_32\nProduces Boolean\nImplements\n Give A / B < A\nEnd F\n", ["udiv i32", "icmp ult i32"])
positive["signed_ops"] = ("Block F Takes\n A : Signed_32\n B : Signed_32\nProduces Boolean\nImplements\n Give A / B < A\nEnd F\n", ["sdiv i32", "icmp slt i32"])
positive["semicolon_and_comments"] = ("Block Main # hi\nDeclares\n X : Signed_32\nImplements\n X = 1; // line\nEnd Main\n", ["define void @Main"])
positive["innermost_exit_skip"] = ("Block Main\nImplements\n Loop\n  Loop\n   Exit\n   Skip\n  End Loop\n  Skip\n End Loop\nEnd Main\n", ["loop.header", "loop.exit"])
positive["bitwise_and_or_xor_not"] = ("Block Bits Takes\n A : Signed_32\n B : Signed_32\nProduces Signed_32\nImplements\n Give (A & B) | (A ^ B) | ~A\nEnd Bits\n", ["and i32", "or i32", "xor i32"])
positive["shifts_lsl_lsr"] = ("Block Shifts Takes\n A : Signed_32\n B : Unsigned_32\nProduces Signed_32\nImplements\n Give (A LSL 2) + ((A LSR 1) & 255)\nEnd Shifts\n", ["shl i32", "ashr i32", "and i32"])
positive["shifts_unsigned_lsr"] = ("Block UShifts Takes\n A : Unsigned_32\nProduces Unsigned_32\nImplements\n Give A LSR 2\nEnd UShifts\n", ["lshr i32"])
positive["modulo_operators"] = ("Block Rem Takes\n A : Signed_32\n B : Signed_32\nProduces Signed_32\nImplements\n Give (A Mod 10) + (B % 4)\nEnd Rem\n", ["srem i32"])
positive["else_if_ladder"] = ("Block Classify Takes\n Score : Signed_32\nProduces Signed_32\nImplements\n If Score >= 90\n  Give 1\n Else If Score >= 75\n  Give 2\n Else If Score >= 50\n  Give 3\n Else\n  Give 4\n End If\nEnd Classify\n", ["if.then", "if.else", "ret i32 1", "ret i32 2", "ret i32 3", "ret i32 4"])
positive["pointer_syntax_postfix"] = ("Block Main Declares\n X : Signed_32 = 42\n Ptr : Signed_32 Pointer = Address(X)\nImplements\n Write(Deref(Ptr))\nEnd Main\n", ["alloca i32", "store ptr", "call i32 (ptr, ...) @printf"])
positive["array_and_slice"] = ("Block Main Declares\n Arr : Signed_32 Array[3]\n Slc : Signed_32 Slice\nImplements\n Arr[1] = 10\n Slc = Slice(Arr)\n Write(Slc[1])\n Write(Slc.Length)\nEnd Main\n", ["alloca [3 x i32]", "alloca { ptr, i64 }", "getelementptr"])
positive["string_indexing_returns_byte"] = ("Block Main Declares\n S : String = \"Loom\"\n First : Unsigned_8\nImplements\n First = S[1]\n Write(First)\n Write(S.Length)\nEnd Main\n", ["str.byte", "call i64 @strlen"])
positive["character_cast_valid_scalar"] = ("Block Main Declares\n C : Character = Cast[Character](233)\n Zero : Character = Cast[Character](-0)\nImplements\n Write(C)\n Write(Zero)\nEnd Main\n", ["store i32 233", "store i32 0"])
positive["default_uninitialized_null_pointer"] = ("Block Main Declares\n Ptr : Signed_32 Pointer\nImplements\n Write(Ptr)\nEnd Main\n", ["ptr.null", "Null"])
positive["ensure_statement"] = ("Block Main Implements\n Ensure { Write(1) }\n Write(2)\nEnd Main\n", ["call i32 (ptr, ...) @printf"])
positive["cast_widening"] = ("Block Main Declares\n Small : Signed_16 = 12\n Big : Signed_64\nImplements\n Big = Cast[Signed_64](Small)\n Write(Big)\nEnd Main\n", ["sext i16", "to i64"])
positive["null_pointer_and_short_circuit"] = ("Block Main Declares\n Ptr : Signed_32 Pointer = Null\nImplements\n If Ptr != Null And Deref(Ptr) == 100\n  Write(1)\n End If\nEnd Main\n", ["icmp ne ptr", "and.rhs", "and.merge"])
positive["extern_block_puts"] = ("Extern Block puts Takes\n S : String\nProduces Signed_32\nEnd puts\nBlock Main Declares\n Res : Signed_32\nImplements\n Res = puts(\"Hello\")\nEnd Main\n", ["declare i32 @puts(ptr)", "call i32 @puts"])
positive["array_literal_initializer"] = ("Block Main Declares\n Arr : Signed_32 Array[3] = {10, 20, 30}\nImplements\n Write(Arr[1])\n Write(Arr[2])\n Write(Arr[3])\nEnd Main\n", ["alloca [3 x i32]"])
positive["write_string_interpolation"] = ("Record Vec\n X : Signed_32\n Y : Signed_32\nEnd Vec\nBlock Main Declares\n A : Signed_32 = 42\n V : Vec\nImplements\n V.X = 10\n V.Y = 20\n Write(\"Val: <A> | Vec: <V.X>, <V.Y>\")\nEnd Main\n", ["printf"])
positive["string_null_escape"] = ("Block Main Declares\n S : String = \"abc\\0def\"\nImplements\n Write(S)\nEnd Main\n", ["@.loom.string"])

negative = {
    "retired_defer_keyword": ("Block Main Implements\n Defer Write(1)\nEnd Main\n", "expected '=' in assignment"),
    "ensure_requires_braces": ("Block Main Implements\n Ensure Write(1)\nEnd Main\n", "expected '{' after Ensure"),
    "incomplete_header": ("Block\n", "expected function name after Block"),
    "missing_nested_end": ("Block A Implements\n If True\nEnd A\n", "expected 'End If'"),
    "missing_end": ("Block Main\nImplements\n", "expected 'End Main'"),
    "mismatched_end": ("Block Main\nImplements\nEnd Other\n", "mismatched block ending"),
    "unexpected_end": ("End Main\n", "unexpected End"),
    "duplicate_function": ("Block A\nImplements\nEnd A\nBlock A\nImplements\nEnd A\n", "duplicate function"),
    "bitwise_non_integer": ("Block Main\nDeclares\n F : Float_32 = 1.0\nImplements\n F = F & 1.0\nEnd Main\n", "operator '&' requires integer operands"),
    "shift_non_integer": ("Block Main\nDeclares\n F : Float_32 = 1.0\nImplements\n F = F LSL 2\nEnd Main\n", "shift operator 'LSL' requires integer operands"),
    "duplicate_param": ("Block A Takes\n X : Signed_32\n X : Signed_32\nImplements\nEnd A\n", "duplicate parameter"),
    "duplicate_local": ("Block A Declares\n X : Signed_32\n X : Signed_32\nImplements\nEnd A\n", "duplicate local"),
    "parameter_local_collision": ("Block A Takes\n X : Signed_32\nDeclares\n X : Signed_32\nImplements\nEnd A\n", "duplicate local or parameter/local"),
    "undeclared_variable": ("Block A Declares\n X : Signed_32\nImplements\n X = Y\nEnd A\n", "undeclared variable 'Y'"),
    "assignment_mismatch": ("Block A Declares\n X : Signed_32\nImplements\n X = True\nEnd A\n", "expected Signed_32"),
    "invalid_if": ("Block A Implements\n If 3\n End If\nEnd A\n", "expected Boolean"),
    "invalid_operator": ("Block A Produces Boolean\nImplements\n Give True + False == True\nEnd A\n", "arithmetic requires numeric"),
    "give_in_procedure": ("Block A Implements\n Give 2\nEnd A\n", "Give is only valid"),
    "missing_give": ("Block A Produces Signed_32\nImplements\n If False\n Give 1\n End If\nEnd A\n", "reachable path without Give"),
    "exit_in_value_block": ("Block A Produces Signed_32 Implements\n Exit\nEnd A\n", "Exit cannot be used in a value-producing block"),
    "skip_outside_loop": ("Block A Implements\n Skip\nEnd A\n", "Skip is only valid inside a Loop"),
    "unsupported_type": ("Block A Produces Decimal128\nImplements\n Give 1\nEnd A\n", "unsupported type 'Decimal128'"),
    "malformed_expression": ("Block A Produces Signed_32\nImplements\n Give 1 +\nEnd A\n", "expected expression"),
    "overflowing_literal": ("Block A Produces Unsigned_64\nImplements\n Give 18446744073709551616\nEnd A\n", "integer literal is too large"),
    "return_not_keyword": ("Block A Implements\n Return\nEnd A\n", "unsupported keyword 'Return'"),
    "case_sensitive_identifier": ("Block A Declares\n X : Signed_32\nImplements\n x = 1\nEnd A\n", "assignment to undeclared variable 'x'"),
    "parenthesized_takes_rejected": ("Block A Takes (X: Signed_32) Implements\nEnd A\n", "parenthesized Takes syntax is not part of Loom"),
    "incompatible_signedness": ("Block A Takes\n X : Signed_32\n Y : Unsigned_32\nProduces Signed_32\nImplements\n Give X + Y\nEnd A\n", "expected Signed_32, found Unsigned_32"),
    "initializer_type_mismatch": ("Block Main Declares\n Value : Signed_32 = True\nImplements\nEnd Main\n", "expected Signed_32, found Boolean"),
    "unresolved_call": ("Block Main Declares\n X : Signed_32\nImplements\n X = Missing(1)\nEnd Main\n", "unresolved block call 'Missing'"),
    "call_keyword_function_rejected": ("Block Add Takes\n X : Signed_32\nProduces Signed_32\nImplements\n Give X + 1\nEnd Add\nBlock Main Declares\n Y : Signed_32\nImplements\n Y = Call Add(41)\nEnd Main\n", "the 'call' keyword has been removed"),
    "call_keyword_procedure_rejected": ("Block Log Implements\n Write(\"hi\")\nEnd Log\nBlock Main Implements\n Call Log()\nEnd Main\n", "the 'call' keyword has been removed"),
    "keyword_case_sensitivity": ("block Main Implements\nEnd Main\n", "expected Block"),
    "float32_without_underscore": ("Block Main Declares\n F : Float32\nImplements\nEnd Main\n", "unsupported type 'Float32'"),
    "deref_non_pointer": ("Block Main Declares\n X : Signed_32 = 5\nImplements\n Write(Deref(X))\nEnd Main\n", "Deref() requires a pointer type"),
    "wrong_call_arity": ("Block A Takes\n X : Signed_32\nProduces Signed_32\nImplements\n Give X\nEnd A\nBlock Main Declares\n Y : Signed_32\nImplements\n Y = A()\nEnd Main\n", "expects 1 arguments, got 0"),
    "wrong_call_argument_type": ("Block A Takes\n X : Boolean\nProduces Signed_32\nImplements\n Give 1\nEnd A\nBlock Main Declares\n Y : Signed_32\nImplements\n Y = A(2)\nEnd Main\n", "expected Boolean"),
    "procedure_as_value": ("Block P Implements\nEnd P\nBlock F Produces Signed_32\nImplements\n Give P()\nEnd F\n", "cannot be used as a value"),
    "else_missing_return": ("Block Choose Takes\n B : Boolean\nProduces Signed_32\nImplements\n If B\n  Give 1\n Else\n  Write(\"no return\")\n End If\nEnd Choose\n", "reachable path without Give"),
    "invalid_loop_until_type": ("Block Main Implements\n Loop Until 4\n End Loop\nEnd Main\n", "expected Boolean, found Signed_32"),
    "constant_global_assignment": ("Block Global\nDeclares\n Limit : Constant Signed_32 = 4\nEnd Global\nBlock Main Implements\n Limit = 5\nEnd Main\n", "cannot assign to constant 'Limit'"),
    "constant_local_assignment": ("Block Main Declares\n Local_Const : Constant Signed_32 = 4\nImplements\n Local_Const = 5\nEnd Main\n", "cannot assign to constant 'Local_Const'"),
    "global_initializer_not_constant": ("Block Global\nDeclares\n Base : Signed_32 = 4\n Next : Signed_32 = Base + 1\nEnd Global\nBlock Main Implements\nEnd Main\n", "global initializers must be literal constants"),
    "local_shadows_global": ("Block Global\nDeclares\n Value : Signed_32\nEnd Global\nBlock Main Declares\n Value : Signed_32\nImplements\nEnd Main\n", "shadows a global"),
    "enum_out_of_range": ("Enum Tiny : Unsigned_8\n Too_Big = 256\nEnd Tiny\nBlock Main Implements\nEnd Main\n", "enum value does not fit Unsigned_8"),
    "enum_duplicate_value": ("Enum Mode : Unsigned_8\n A = 1\n B = 1\nEnd Mode\nBlock Main Implements\nEnd Main\n", "duplicate numeric value"),
    "enum_incompatible": ("Enum Mode : Unsigned_8\n A = 1\nEnd Mode\nBlock Main Declares\n N : Unsigned_8\n M : Mode\nImplements\n M = N\nEnd Main\n", "expected Mode, found Unsigned_8"),
    "record_missing_field": ("Record R\n X : Signed_32\nEnd R\nBlock Main Declares\n V : R\nImplements\n V.Nope = 1\nEnd Main\n", "has no field 'Nope'"),
    "duplicate_record_field": ("Record R\n X : Signed_32\n X : Signed_32\nEnd R\nBlock Main Implements\nEnd Main\n", "duplicate field 'X'"),
    "record_field_type_mismatch": ("Record R\n X : Boolean\nEnd R\nBlock Main Declares\n V : R\nImplements\n V.X = 1\nEnd Main\n", "expected Boolean, found Signed_32"),
    "cast_pointer_to_narrow_int": ("Block Main Declares\n X : Signed_32 = 1\n P : Signed_32 Pointer = Address(X)\n N : Signed_8\nImplements\n N = Cast[Signed_8](P)\nEnd Main\n", "cannot cast 64-bit pointer to 8-bit integer"),
    "ensure_give_rejected": ("Block Main Produces Signed_32 Implements\n Ensure { Give 1 }\n Give 2\nEnd Main\n", "control-flow statements are not valid inside ensure"),
    "array_index_not_integer": ("Block Main Declares\n Arr : Signed_32 Array[3]\nImplements\n Arr[\"zero\"] = 1\nEnd Main\n", "array/slice index must be an integer"),
    "missing_module_header": ("Block Main Implements\nEnd Main\n", "Every Loom file must begin with 'Module <module_name>' at the top of the file"),
    "array_index_zero_rejected": ("Block Main Declares\n Arr : Signed_32 Array[3]\nImplements\n Arr[0] = 1\nEnd Main\n", "1-based indexing requires indices >= 1; index 0 is out of bounds"),
    "array_index_negative_rejected": ("Block Main Declares\n Arr : Signed_32 Array[3]\nImplements\n Arr[-1] = 1\nEnd Main\n", "1-based indexing requires indices >= 1; negative indices are out of bounds"),
    "array_index_exceeds_bounds": ("Block Main Declares\n Arr : Signed_32 Array[3]\nImplements\n Arr[4] = 1\nEnd Main\n", "exceeds array length 3"),
    "string_index_is_not_character": ("Block Main Declares\n S : String = \"é\"\n C : Character\nImplements\n C = S[1]\nEnd Main\n", "expected Character, found Unsigned_8"),
    "string_index_assignment_rejected": ("Block Main Declares\n S : String = \"hi\"\nImplements\n S[1] = 'H'\nEnd Main\n", "String characters are read-only"),
    "null_is_not_a_declarable_type": ("Block Main Declares\n N : Null\nImplements\nEnd Main\n", "'Null' is a pointer literal, not a declarable type"),
    "array_literal_size_mismatch": ("Block Main Declares\n Arr : Signed_32 Array[3] = {1, 2}\nImplements\nEnd Main\n", "array literal has 2 elements, but expected 3"),
    "array_size_zero_rejected": ("Block Main Declares\n Arr : Signed_32 Array[0]\nImplements\nEnd Main\n", "array size must be greater than zero, found 0"),
    "character_cast_rejects_surrogate": ("Block Main Declares\n C : Character = Cast[Character](55296)\nImplements\nEnd Main\n", "not a valid Unicode scalar value"),
}

def ensure_module(source: str, name: str) -> str:
    lines = source.strip().splitlines()
    for l in lines:
        s = l.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("Module "):
            return source
        break
    return f"Module {name}\n\n" + source

with tempfile.TemporaryDirectory(prefix="loom-tests-") as td:
    td = pathlib.Path(td)
    for name, (source, needles) in positive.items():
        src, out = td / f"{name}.loom", td / f"{name}.ll"
        src.write_text(ensure_module(source, name))
        run = subprocess.run([str(compiler), str(src), str(out)], text=True, capture_output=True)
        if run.returncode:
            raise SystemExit(f"FAIL {name}: expected success\n{run.stderr}")
        ir = out.read_text()
        for needle in needles:
            if needle not in ir:
                raise SystemExit(f"FAIL {name}: needle '{needle}' not found in emitted LLVM IR:\n{ir}")
        if name == "precedence_parentheses":
            order = [ir.find(op) for op in ("add i32", "mul i32", "sdiv i32", "sub i32")]
            if order != sorted(order): raise SystemExit(f"FAIL {name}: emitted operator order does not reflect precedence")
        if name == "left_associative_subtraction" and ir.count("sub i32") != 2:
            raise SystemExit("FAIL left_associative_subtraction: expected two subtraction operations")
    for name, (source, expected) in negative.items():
        src = td / f"{name}.loom"
        src_text = source if name == "missing_module_header" else ensure_module(source, name)
        src.write_text(src_text)
        run = subprocess.run([str(compiler), str(src)], text=True, capture_output=True)
        if run.returncode == 0: raise SystemExit(f"FAIL {name}: expected compiler rejection")
        if expected.lower() not in run.stderr.lower(): raise SystemExit(f"FAIL {name}: expected diagnostic {expected!r}, got {run.stderr!r}")
    narrow_src = td / "narrow_cast.loom"
    narrow_src.write_text("Module NarrowCast\n\nBlock Main Declares\n Big : Signed_64 = 10\n Small : Signed_32\nImplements\n Small = Cast[Signed_32](Big)\nEnd Main\n")
    narrow_run = subprocess.run([str(compiler), str(narrow_src), str(td / "narrow.ll")], text=True, capture_output=True)
    if narrow_run.returncode != 0 or "narrowing cast" not in narrow_run.stderr.lower():
        raise SystemExit(f"FAIL narrow_cast warning check: expected warning, got: {narrow_run.stderr}")
# CLI path failures must be reported as compiler errors, not successful empty output.
    missing = subprocess.run([str(compiler), str(td / "no-such-input.loom")], text=True, capture_output=True)
    if missing.returncode == 0 or "cannot open input file" not in missing.stderr.lower():
        raise SystemExit(f"FAIL missing_input: unexpected result {missing.returncode}: {missing.stderr}")
    output_error = subprocess.run([str(compiler), str(canonical), str(td / "missing-dir" / "out.ll")], text=True, capture_output=True)
    if output_error.returncode == 0 or "cannot write output file" not in output_error.stderr.lower():
        raise SystemExit(f"FAIL output_error: unexpected result {output_error.returncode}: {output_error.stderr}")
    bad_character = td / "bad character.loom"
    bad_character.write_text("Module BadChar\nBlock Main\nImplements\n   @\nEnd Main\n")
    diagnostic = subprocess.run([str(compiler), str(bad_character)], text=True, capture_output=True)
    expected_location = f"{bad_character}:4:4"
    if diagnostic.returncode == 0 or "error[E0001]" not in diagnostic.stderr or expected_location not in diagnostic.stderr or "4 |    @" not in diagnostic.stderr or "^" not in diagnostic.stderr:
        raise SystemExit(f"FAIL source_diagnostic_location: expected an error with path, line 4, column 4, and caret; got:\n{diagnostic.stderr}")
    if diagnostic.stderr.count("error[") != 1 or "loomc:" in diagnostic.stderr:
        raise SystemExit(f"FAIL diagnostic_format: unexpected duplicate/generic compiler error:\n{diagnostic.stderr}")
    unicode_file = td / "unicode-column.loom"
    unicode_file.write_text('Module UnicodeCol\nBlock Main\nImplements\n    Write("🚀") @\nEnd Main\n')
    unicode_diagnostic = subprocess.run([str(compiler), str(unicode_file)], text=True, capture_output=True)
    if unicode_diagnostic.returncode == 0 or f"{unicode_file}:4:17" not in unicode_diagnostic.stderr:
        raise SystemExit(f"FAIL unicode_column: non-BMP characters should count as two UTF-16 columns; got:\n{unicode_diagnostic.stderr}")
    semantic_file = td / "semantic-location.loom"
    semantic_file.write_text("Module SemanticLoc\nBlock Main\nImplements\n    Missing = 1\nEnd Main\n")
    semantic = subprocess.run([str(compiler), str(semantic_file)], text=True, capture_output=True)
    if semantic.returncode == 0 or f"{semantic_file}:4:5" not in semantic.stderr or "undeclared variable 'Missing'" not in semantic.stderr:
        raise SystemExit(f"FAIL semantic_diagnostic_location: expected the unknown name's exact column; got:\n{semantic.stderr}")
    valid_part = td / "first part.loom"
    invalid_part = td / "second part.loom"
    valid_part.write_text("Module HelperPart\nBlock Helper\nImplements\nEnd Helper\n")
    invalid_part.write_text("Module MainPart\nBlock Main\nImplements\n    Absent = 1\nEnd Main\n")
    multi = subprocess.run([str(compiler), "--graph-data", "--output", str(td / "multi.tsv"), str(valid_part), str(invalid_part)], text=True, capture_output=True)
    if multi.returncode == 0 or f"{invalid_part}:4:5" not in multi.stderr:
        raise SystemExit(f"FAIL multi_source_diagnostic: expected the second file's location; got:\n{multi.stderr}")
    tab_file = td / "tab-location.loom"
    tab_file.write_text("Module TabLocation\nBlock Main\nImplements\n\t@\nEnd Main\n")
    tabbed = subprocess.run([str(compiler), str(tab_file)], text=True, capture_output=True)
    if tabbed.returncode == 0 or f"{tab_file}:4:5" not in tabbed.stderr:
        raise SystemExit(f"FAIL tab_diagnostic_location: tabs should advance to the next 4-column stop; got:\n{tabbed.stderr}")
    eof_source = td / "eof-error.loom"
    eof_source.write_text("Module EofTest\nBlock Main\nImplements\n")
    eof_result = subprocess.run([str(compiler), str(eof_source)], text=True, capture_output=True)
    if eof_result.returncode == 0 or f"{eof_source}:4:" not in eof_result.stderr or "expected 'end main'" not in eof_result.stderr.lower():
        raise SystemExit(f"FAIL eof_diagnostic: expected a source-located end-of-file error; got:\n{eof_result.stderr}")
    removed_mode = subprocess.run([str(compiler), "--freestanding", str(canonical)], text=True, capture_output=True)
    if removed_mode.returncode == 0 or "unknown option '--freestanding'" not in removed_mode.stderr:
        raise SystemExit(f"FAIL removed_mode: expected --freestanding to be rejected as an unsupported option; got:\n{removed_mode.stderr}")
print(f"passed {len(positive)} positive and {len(negative)} negative Loom frontend cases plus CLI I/O checks")
