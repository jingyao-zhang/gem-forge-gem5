microcode = '''

def macroop VPERMILPD_XMM_XMM_I {
    mpermilpd dest=xmm0, src1=xmm0m, op2=xmm1m, size=8, ext="(IMMEDIATE >> 0) & 0x1"
    mpermilpd dest=xmm1, src1=xmm0m, op2=xmm1m, size=8, ext="(IMMEDIATE >> 1) & 0x1"
    vclear dest=xmm2, destVL=16
};

def macroop VPERMILPD_XMM_M_I {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    mpermilpd dest=xmm0, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 0) & 0x1"
    mpermilpd dest=xmm1, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 1) & 0x1"
    vclear dest=xmm2, destVL=16
};

def macroop VPERMILPD_XMM_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    mpermilpd dest=xmm0, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 0) & 0x1"
    mpermilpd dest=xmm1, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 1) & 0x1"
    vclear dest=xmm2, destVL=16
};

def macroop VPERMILPD_YMM_YMM_I {
    mpermilpd dest=xmm0, src1=xmm0m, op2=xmm1m, size=8, ext="(IMMEDIATE >> 0) & 0x1"
    mpermilpd dest=xmm1, src1=xmm0m, op2=xmm1m, size=8, ext="(IMMEDIATE >> 1) & 0x1"
    mpermilpd dest=xmm2, src1=xmm2m, op2=xmm3m, size=8, ext="(IMMEDIATE >> 2) & 0x1"
    mpermilpd dest=xmm3, src1=xmm2m, op2=xmm3m, size=8, ext="(IMMEDIATE >> 3) & 0x1"
    vclear dest=xmm4, destVL=32
};

def macroop VPERMILPD_YMM_M_I {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    mpermilpd dest=xmm0, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 0) & 0x1"
    mpermilpd dest=xmm1, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 1) & 0x1"
    mpermilpd dest=xmm2, src1=ufp3, op2=ufp4, size=8, ext="(IMMEDIATE >> 2) & 0x1"
    mpermilpd dest=xmm3, src1=ufp3, op2=ufp4, size=8, ext="(IMMEDIATE >> 3) & 0x1"
    vclear dest=xmm4, destVL=32
};

def macroop VPERMILPD_YMM_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    mpermilpd dest=xmm0, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 0) & 0x1"
    mpermilpd dest=xmm1, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 1) & 0x1"
    mpermilpd dest=xmm2, src1=ufp3, op2=ufp4, size=8, ext="(IMMEDIATE >> 2) & 0x1"
    mpermilpd dest=xmm3, src1=ufp3, op2=ufp4, size=8, ext="(IMMEDIATE >> 3) & 0x1"
    vclear dest=xmm4, destVL=32
};

def macroop VPERMILPD_ZMM_ZMM_I {
    mpermilpd dest=xmm0, src1=xmm0m, op2=xmm1m, size=8, ext="(IMMEDIATE >> 0) & 0x1"
    mpermilpd dest=xmm1, src1=xmm0m, op2=xmm1m, size=8, ext="(IMMEDIATE >> 1) & 0x1"
    mpermilpd dest=xmm2, src1=xmm2m, op2=xmm3m, size=8, ext="(IMMEDIATE >> 2) & 0x1"
    mpermilpd dest=xmm3, src1=xmm2m, op2=xmm3m, size=8, ext="(IMMEDIATE >> 3) & 0x1"
    mpermilpd dest=xmm4, src1=xmm4m, op2=xmm5m, size=8, ext="(IMMEDIATE >> 4) & 0x1"
    mpermilpd dest=xmm5, src1=xmm4m, op2=xmm5m, size=8, ext="(IMMEDIATE >> 5) & 0x1"
    mpermilpd dest=xmm6, src1=xmm6m, op2=xmm7m, size=8, ext="(IMMEDIATE >> 6) & 0x1"
    mpermilpd dest=xmm7, src1=xmm6m, op2=xmm7m, size=8, ext="(IMMEDIATE >> 7) & 0x1"
};

def macroop VPERMILPD_ZMM_M_I {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, sib, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, sib, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, sib, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, sib, "DISPLACEMENT + 56", dataSize=8
    mpermilpd dest=xmm0, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 0) & 0x1"
    mpermilpd dest=xmm1, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 1) & 0x1"
    mpermilpd dest=xmm2, src1=ufp3, op2=ufp4, size=8, ext="(IMMEDIATE >> 2) & 0x1"
    mpermilpd dest=xmm3, src1=ufp3, op2=ufp4, size=8, ext="(IMMEDIATE >> 3) & 0x1"
    mpermilpd dest=xmm4, src1=ufp5, op2=ufp6, size=8, ext="(IMMEDIATE >> 4) & 0x1"
    mpermilpd dest=xmm5, src1=ufp5, op2=ufp6, size=8, ext="(IMMEDIATE >> 5) & 0x1"
    mpermilpd dest=xmm6, src1=ufp7, op2=ufp8, size=8, ext="(IMMEDIATE >> 6) & 0x1"
    mpermilpd dest=xmm7, src1=ufp7, op2=ufp8, size=8, ext="(IMMEDIATE >> 7) & 0x1"
};

def macroop VPERMILPD_ZMM_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    ldfp ufp5, seg, riprel, "DISPLACEMENT + 32", dataSize=8
    ldfp ufp6, seg, riprel, "DISPLACEMENT + 40", dataSize=8
    ldfp ufp7, seg, riprel, "DISPLACEMENT + 48", dataSize=8
    ldfp ufp8, seg, riprel, "DISPLACEMENT + 56", dataSize=8
    mpermilpd dest=xmm0, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 0) & 0x1"
    mpermilpd dest=xmm1, src1=ufp1, op2=ufp2, size=8, ext="(IMMEDIATE >> 1) & 0x1"
    mpermilpd dest=xmm2, src1=ufp3, op2=ufp4, size=8, ext="(IMMEDIATE >> 2) & 0x1"
    mpermilpd dest=xmm3, src1=ufp3, op2=ufp4, size=8, ext="(IMMEDIATE >> 3) & 0x1"
    mpermilpd dest=xmm4, src1=ufp5, op2=ufp6, size=8, ext="(IMMEDIATE >> 4) & 0x1"
    mpermilpd dest=xmm5, src1=ufp5, op2=ufp6, size=8, ext="(IMMEDIATE >> 5) & 0x1"
    mpermilpd dest=xmm6, src1=ufp7, op2=ufp8, size=8, ext="(IMMEDIATE >> 6) & 0x1"
    mpermilpd dest=xmm7, src1=ufp7, op2=ufp8, size=8, ext="(IMMEDIATE >> 7) & 0x1"
};
'''