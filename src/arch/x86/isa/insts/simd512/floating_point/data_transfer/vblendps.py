microcode = '''

def macroop VBLENDPS_XMM_XMM_I {
    mblend dest=xmm0, src1=xmm0v, op2=xmm0m, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mblend dest=xmm1, src1=xmm1v, op2=xmm1m, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VBLENDPS_XMM_M_I {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    mblend dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mblend dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VBLENDPS_XMM_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    mblend dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mblend dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VBLENDPS_YMM_YMM_I {
    mblend dest=xmm0, src1=xmm0v, op2=xmm0m, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mblend dest=xmm1, src1=xmm1v, op2=xmm1m, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    mblend dest=xmm2, src1=xmm2v, op2=xmm2m, size=4, ext="(IMMEDIATE >> 4) & 0x3"
    mblend dest=xmm3, src1=xmm3v, op2=xmm3m, size=4, ext="(IMMEDIATE >> 6) & 0x3"
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VBLENDPS_YMM_M_I {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    mblend dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mblend dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    mblend dest=xmm2, src1=xmm2v, op2=ufp3, size=4, ext="(IMMEDIATE >> 4) & 0x3"
    mblend dest=xmm3, src1=xmm3v, op2=ufp4, size=4, ext="(IMMEDIATE >> 6) & 0x3"
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VBLENDPS_YMM_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    mblend dest=xmm0, src1=xmm0v, op2=ufp1, size=4, ext="(IMMEDIATE >> 0) & 0x3"
    mblend dest=xmm1, src1=xmm1v, op2=ufp2, size=4, ext="(IMMEDIATE >> 2) & 0x3"
    mblend dest=xmm2, src1=xmm2v, op2=ufp3, size=4, ext="(IMMEDIATE >> 4) & 0x3"
    mblend dest=xmm3, src1=xmm3v, op2=ufp4, size=4, ext="(IMMEDIATE >> 6) & 0x3"
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

'''