microcode = '''
def macroop VMOVAPD_XMM_XMM {
    movfp dest=xmm0, src1=xmm0m, dataSize=8
    movfp dest=xmm1, src1=xmm1m, dataSize=8
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMOVAPD_XMM_M {
    ldfp xmm0, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm1, seg, sib, "DISPLACEMENT + 8", dataSize=8
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMOVAPD_XMM_P {
    rdip t7
    ldfp xmm0, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm1, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMOVAPD_M_XMM {
    stfp xmm0, seg, sib, "DISPLACEMENT + 0", dataSize=8
    stfp xmm1, seg, sib, "DISPLACEMENT + 8", dataSize=8
};

def macroop VMOVAPD_P_XMM {
    rdip t7
    stfp xmm0, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    stfp xmm1, seg, riprel, "DISPLACEMENT + 8", dataSize=8
};

def macroop VMOVAPD_YMM_YMM {
    movfp dest=xmm0, src1=xmm0m, dataSize=8
    movfp dest=xmm1, src1=xmm1m, dataSize=8
    movfp dest=xmm2, src1=xmm2m, dataSize=8
    movfp dest=xmm3, src1=xmm3m, dataSize=8
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMOVAPD_YMM_M {
    ldfp xmm0, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm1, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp xmm2, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp xmm3, seg, sib, "DISPLACEMENT + 24", dataSize=8
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMOVAPD_YMM_P {
    rdip t7
    ldfp xmm0, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm1, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp xmm2, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp xmm3, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMOVAPD_M_YMM {
    stfp xmm0, seg, sib, "DISPLACEMENT + 0", dataSize=8
    stfp xmm1, seg, sib, "DISPLACEMENT + 8", dataSize=8
    stfp xmm2, seg, sib, "DISPLACEMENT + 16", dataSize=8
    stfp xmm3, seg, sib, "DISPLACEMENT + 24", dataSize=8
};

def macroop VMOVAPD_P_YMM {
    rdip t7
    stfp xmm0, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    stfp xmm1, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    stfp xmm2, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    stfp xmm3, seg, riprel, "DISPLACEMENT + 24", dataSize=8
};

def macroop VMOVAPD_ZMM_ZMM {
    movfp dest=xmm0, src1=xmm0m, dataSize=8
    movfp dest=xmm1, src1=xmm1m, dataSize=8
    movfp dest=xmm2, src1=xmm2m, dataSize=8
    movfp dest=xmm3, src1=xmm3m, dataSize=8
    movfp dest=xmm4, src1=xmm4m, dataSize=8
    movfp dest=xmm5, src1=xmm5m, dataSize=8
    movfp dest=xmm6, src1=xmm6m, dataSize=8
    movfp dest=xmm7, src1=xmm7m, dataSize=8
};

def macroop VMOVAPD_ZMM_M {
    ldfp xmm0, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm1, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp xmm2, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp xmm3, seg, sib, "DISPLACEMENT + 24", dataSize=8
    ldfp xmm4, seg, sib, "DISPLACEMENT + 32", dataSize=8
    ldfp xmm5, seg, sib, "DISPLACEMENT + 40", dataSize=8
    ldfp xmm6, seg, sib, "DISPLACEMENT + 48", dataSize=8
    ldfp xmm7, seg, sib, "DISPLACEMENT + 56", dataSize=8
};

def macroop VMOVAPD_ZMM_P {
    rdip t7
    ldfp xmm0, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm1, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp xmm2, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp xmm3, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    ldfp xmm4, seg, riprel, "DISPLACEMENT + 32", dataSize=8
    ldfp xmm5, seg, riprel, "DISPLACEMENT + 40", dataSize=8
    ldfp xmm6, seg, riprel, "DISPLACEMENT + 48", dataSize=8
    ldfp xmm7, seg, riprel, "DISPLACEMENT + 56", dataSize=8
};

def macroop VMOVAPD_M_ZMM {
    stfp xmm0, seg, sib, "DISPLACEMENT + 0", dataSize=8
    stfp xmm1, seg, sib, "DISPLACEMENT + 8", dataSize=8
    stfp xmm2, seg, sib, "DISPLACEMENT + 16", dataSize=8
    stfp xmm3, seg, sib, "DISPLACEMENT + 24", dataSize=8
    stfp xmm4, seg, sib, "DISPLACEMENT + 32", dataSize=8
    stfp xmm5, seg, sib, "DISPLACEMENT + 40", dataSize=8
    stfp xmm6, seg, sib, "DISPLACEMENT + 48", dataSize=8
    stfp xmm7, seg, sib, "DISPLACEMENT + 56", dataSize=8
};

def macroop VMOVAPD_P_ZMM {
    rdip t7
    stfp xmm0, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    stfp xmm1, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    stfp xmm2, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    stfp xmm3, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    stfp xmm4, seg, riprel, "DISPLACEMENT + 32", dataSize=8
    stfp xmm5, seg, riprel, "DISPLACEMENT + 40", dataSize=8
    stfp xmm6, seg, riprel, "DISPLACEMENT + 48", dataSize=8
    stfp xmm7, seg, riprel, "DISPLACEMENT + 56", dataSize=8
};

'''