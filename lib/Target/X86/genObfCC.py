import random

r32_8 =  [ "AH",  "SIL",  "DIL",  "BH",  "BPL"]
r32_16 = [ "AX",  "SI" ,  "DI" ,  "BX",  "BP" ]
r32_32 = ["EAX", "ESI" , "EDI" , "EBX", "EBP" ]

r64_32 = ["EAX", "ESI" , "EDI" , "R8D", "R9D", "EBX", "EBP" , "R10D", "R11D", "R12D", "R13D", "R14D", "R15D"]
r64_64 = ["RAX", "RSI" , "RDI" , "R8" , "R9" , "RBX", "RBP" , "R10" , "R11" , "R12" , "R13" , "R14" , "R15" ]

retcc = []
cc32_32 = []
cc64_32 = []
cc64_64 = []
csr64 = []

for i in range(10):
    t=list(range(1,len(r32_8)))
    random.shuffle(t)
    retcc.append(t[:3])
    t=list(range(len(r32_8)))
    random.shuffle(t)
    t.remove(random.randrange(1,3))
    cc32_32.append(t)
    t=list(range(5,len(r64_32)))
    t.append(0)
    random.shuffle(t)
    cc64_32.append(t[:4])
    t=list(range(5,len(r64_32)))
    t.append(0)
    random.shuffle(t)
    cc64_64.append(t[:4])
    t=list(range(len(r64_32)))
    for x in retcc[i]:
        if x in t:
            t.remove(x)
        if x+2 in t:
            t.remove(x+2)
    for x in cc64_32[i]:
        if x in t:
            t.remove(x)
    for x in cc64_64[i]:
        if x in t:
            t.remove(x)
    csr64.append(t)


obftd = ""
text = """
def RetCC_OBF_CALL%d : CallingConv<[
  CCIfType<[i8] , CCAssignToReg<[%s, %s, %s]>>,
  CCIfType<[i16], CCAssignToReg<[%s, %s, %s]>>,
  CCIfType<[i32], CCAssignToReg<[%s, %s, %s]>>,
  CCIfType<[i64], CCAssignToReg<[%s, %s, %s]>>,
  CCIfType<[f32, f64, v4i32, v2i64, v4f32, v2f64],
            CCAssignToReg<[XMM3,XMM2,XMM1,XMM0]>>,
  CCDelegateTo<RetCC_X86Common>
]>;
"""
for i in range(10):
    obftd += text % (i, r32_8[retcc[i][0]],r32_8[retcc[i][1]],r32_8[retcc[i][2]],
                        r32_16[retcc[i][0]],r32_16[retcc[i][1]],r32_16[retcc[i][2]],
                        r32_32[retcc[i][0]],r32_32[retcc[i][1]],r32_32[retcc[i][2]],
                        r64_64[retcc[i][0]+2],r64_64[retcc[i][1]+2],r64_64[retcc[i][2]+2])

obftd += """
// This is the return-value convention used for the entire X86 backend.
let Entry = 1 in
def RetCC_X86 : CallingConv<[

  // Check if this is the Intel OpenCL built-ins calling convention
  CCIfCC<"CallingConv::Intel_OCL_BI", CCDelegateTo<RetCC_Intel_OCL_BI>>,
"""
text = """
  CCIfCC<"CallingConv::OBF_CALL%d", CCDelegateTo<RetCC_OBF_CALL%d>>,
"""
for i in range(10):
    obftd += text % (i, i)

obftd += """
  CCIfSubtarget<"is64Bit()", CCDelegateTo<RetCC_X86_64>>,
  CCDelegateTo<RetCC_X86_32>
]>;
"""

text = """
def CC_OBF_CALL%d : CallingConv<[
  CCIfType<[i1, i8, i16], CCPromoteToType<i32>>,
  CCIfType<[i32], CCIfSubtarget<"is64Bit()", CCAssignToReg<[%s, %s, %s, %s]>>>,
  CCIfType<[i64], CCIfSubtarget<"is64Bit()", CCAssignToReg<[%s, %s, %s, %s]>>>,
  CCIfType<[i32], CCAssignToReg<[%s, %s, %s, %s]>>,
  CCIfType<[i64], CCAssignToStack<8, 4>>,
  CCIfType<[f32, f64, v4i32, v2i64, v4f32, v2f64],
           CCAssignToReg<[XMM2, XMM3, XMM0, XMM1]>>,
  CCIfSubtarget<"isTargetWin64()", CCDelegateTo<CC_X86_Win64_C>>,
  CCIfSubtarget<"is64Bit()",       CCDelegateTo<CC_X86_64_C>>,
  CCDelegateTo<CC_X86_32_C>
]>;
"""
for i in range(10):
    obftd += text % (i, r64_32[cc64_32[i][0]],r64_32[cc64_32[i][1]],r64_32[cc64_32[i][2]],r64_32[cc64_32[i][3]],
                        r64_64[cc64_64[i][0]],r64_64[cc64_64[i][1]],r64_64[cc64_64[i][2]],r64_64[cc64_64[i][3]],
                        r32_32[cc32_32[i][0]],r32_32[cc32_32[i][1]],r32_32[cc32_32[i][2]],r32_32[cc32_32[i][3]])

obftd += """
// This is the argument convention used for the entire X86 backend.
let Entry = 1 in
def CC_X86 : CallingConv<[
  CCIfCC<"CallingConv::Intel_OCL_BI", CCDelegateTo<CC_Intel_OCL_BI>>,
"""
text = """
  CCIfCC<"CallingConv::OBF_CALL%d", CCDelegateTo<CC_OBF_CALL%d>>,
"""
for i in range(10):
    obftd += text % (i, i)

obftd += """
  CCIfSubtarget<"is64Bit()", CCDelegateTo<CC_X86_64>>,
  CCDelegateTo<CC_X86_32>
]>;
"""

text = """
def CSR_32_OBF%d : CalleeSavedRegs<(add ECX, EDX)>;
def CSR_64_OBF%d : CalleeSavedRegs<(add %s RCX, RDX)>;
"""

for i in range(10):
    template = ""
    for x in csr64[i]:
        template += r64_64[x] + ", "
    obftd += text % (i, i, template)

open("ObfCall.td", "w").write(obftd)