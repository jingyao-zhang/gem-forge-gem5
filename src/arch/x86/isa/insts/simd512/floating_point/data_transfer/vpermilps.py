microcode = '''

def macroop VPERMILPS_XMM_XMM_I {
    mpermilps dest=xmm0, src1=xmm0m, op2=xmm1m, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mpermilps dest=xmm1, src1=xmm0m, op2=xmm1m, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    vclear dest=xmm2, destVL=16
};

def macroop VPERMILPS_XMM_M_I {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    mpermilps dest=xmm0, src1=ufp1, op2=ufp2, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mpermilps dest=xmm1, src1=ufp1, op2=ufp2, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    vclear dest=xmm2, destVL=16
};

def macroop VPERMILPS_XMM_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    mpermilps dest=xmm0, src1=ufp1, op2=ufp2, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mpermilps dest=xmm1, src1=ufp1, op2=ufp2, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    vclear dest=xmm2, destVL=16
};

def macroop VPERMILPS_YMM_YMM_I {
    mpermilps dest=xmm0, src1=xmm0m, op2=xmm1m, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mpermilps dest=xmm1, src1=xmm0m, op2=xmm1m, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    mpermilps dest=xmm2, src1=xmm2m, op2=xmm3m, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mpermilps dest=xmm3, src1=xmm2m, op2=xmm3m, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    vclear dest=xmm4, destVL=32
};

def macroop VPERMILPS_YMM_M_I {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    mpermilps dest=xmm0, src1=ufp1, op2=ufp2, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mpermilps dest=xmm1, src1=ufp1, op2=ufp2, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    mpermilps dest=xmm2, src1=ufp3, op2=ufp4, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mpermilps dest=xmm3, src1=ufp3, op2=ufp4, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    vclear dest=xmm4, destVL=32
};

def macroop VPERMILPS_YMM_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    mpermilps dest=xmm0, src1=ufp1, op2=ufp2, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mpermilps dest=xmm1, src1=ufp1, op2=ufp2, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    mpermilps dest=xmm2, src1=ufp3, op2=ufp4, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mpermilps dest=xmm3, src1=ufp3, op2=ufp4, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    vclear dest=xmm4, destVL=32
};

'''