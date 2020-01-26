
microcode = '''
def macroop VPXOR_XMM_XMM {
    mxor xmm0, xmm0v, xmm0m
    mxor xmm1, xmm1v, xmm1m
    mxor xmm2, xmm2, xmm2
    mxor xmm3, xmm3, xmm3
    mxor xmm4, xmm4, xmm4
    mxor xmm5, xmm5, xmm5
    mxor xmm6, xmm6, xmm6
    mxor xmm7, xmm7, xmm7
};

def macroop VPXOR_XMM_M {
    ldfp ufp1, seg, sib, disp, dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    mxor xmm0, xmm0v, ufp1
    mxor xmm1, xmm1v, ufp2
    mxor xmm2, xmm2, xmm2
    mxor xmm3, xmm3, xmm3
    mxor xmm4, xmm4, xmm4
    mxor xmm5, xmm5, xmm5
    mxor xmm6, xmm6, xmm6
    mxor xmm7, xmm7, xmm7
};

def macroop VPXOR_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, disp, dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    mxor xmm0, xmm0v, ufp1
    mxor xmm1, xmm1v, ufp2
    mxor xmm2, xmm2, xmm2
    mxor xmm3, xmm3, xmm3
    mxor xmm4, xmm4, xmm4
    mxor xmm5, xmm5, xmm5
    mxor xmm6, xmm6, xmm6
    mxor xmm7, xmm7, xmm7
};

def macroop VPXOR_YMM_YMM {
    mxor xmm0, xmm0v, xmm0m
    mxor xmm1, xmm1v, xmm1m
    mxor xmm2, xmm2v, xmm2m
    mxor xmm3, xmm3v, xmm3m
    mxor xmm4, xmm4, xmm4
    mxor xmm5, xmm5, xmm5
    mxor xmm6, xmm6, xmm6
    mxor xmm7, xmm7, xmm7
};

def macroop VPXOR_YMM_M {
    ldfp ufp1, seg, sib, disp, dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, sib, "DISPLACEMENT + 24", dataSize=8
    mxor xmm0, xmm0v, ufp1
    mxor xmm1, xmm1v, ufp2
    mxor xmm2, xmm2v, ufp3
    mxor xmm3, xmm3v, ufp4
    mxor xmm4, xmm4, xmm4
    mxor xmm5, xmm5, xmm5
    mxor xmm6, xmm6, xmm6
    mxor xmm7, xmm7, xmm7
};

def macroop VPXOR_YMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, disp, dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp ufp3, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp ufp4, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    mxor xmm0, xmm0v, ufp1
    mxor xmm1, xmm1v, ufp2
    mxor xmm2, xmm2v, ufp3
    mxor xmm3, xmm3v, ufp4
    mxor xmm4, xmm4, xmm4
    mxor xmm5, xmm5, xmm5
    mxor xmm6, xmm6, xmm6
    mxor xmm7, xmm7, xmm7
};

'''
