microcode = '''

def macroop VINSERTPS_XMM_XMM_I {
    movfp xmm0, xmm0v, dataSize=8
    movfp xmm1, xmm1v, dataSize=8
    minsertpsl dest=xmm0, src1=xmm0m, op2=xmm1m, size=4, ext="(IMMEDIATE & 0xFF)"
    minsertpsh dest=xmm1, src1=xmm0m, op2=xmm1m, size=4, ext="(IMMEDIATE & 0xFF)"
    vclear dest=xmm2, destVL=16
};

def macroop VINSERTPS_XMM_M_I {
    movfp xmm0, xmm0v, dataSize=8
    movfp xmm1, xmm1v, dataSize=8
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=4
    minsertpsl dest=xmm0, src1=ufp1, op2=ufp1, size=4, ext="(IMMEDIATE & 0x3F)"
    minsertpsh dest=xmm1, src1=ufp1, op2=ufp1, size=4, ext="(IMMEDIATE & 0x3F)"
    vclear dest=xmm2, destVL=16
};

def macroop VINSERTPS_XMM_P_I {
    movfp xmm0, xmm0v, dataSize=8
    movfp xmm1, xmm1v, dataSize=8
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=4
    minsertpsl dest=xmm0, src1=ufp1, op2=ufp1, size=4, ext="(IMMEDIATE & 0x3F)"
    minsertpsh dest=xmm1, src1=ufp1, op2=ufp1, size=4, ext="(IMMEDIATE & 0x3F)"
    vclear dest=xmm2, destVL=16
};

'''