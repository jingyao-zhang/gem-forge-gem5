microcode = '''

def macroop VPBROADCASTQ_ZMM_XMM {
    movfp xmm0, xmm0m, dataSize=8
    movfp xmm1, xmm0m, dataSize=8
    movfp xmm2, xmm0m, dataSize=8
    movfp xmm3, xmm0m, dataSize=8
    movfp xmm4, xmm0m, dataSize=8
    movfp xmm5, xmm0m, dataSize=8
    movfp xmm6, xmm0m, dataSize=8
    movfp xmm7, xmm0m, dataSize=8
};

def macroop VPBROADCASTQ_ZMM_M {
    ldfp xmm0, seg, sib, disp, dataSize=8
    movfp xmm1, xmm0, dataSize=8
    movfp xmm2, xmm0, dataSize=8
    movfp xmm3, xmm0, dataSize=8
    movfp xmm4, xmm0, dataSize=8
    movfp xmm5, xmm0, dataSize=8
    movfp xmm6, xmm0, dataSize=8
    movfp xmm7, xmm0, dataSize=8
};

def macroop VPBROADCASTQ_ZMM_P {
    rdip t7
    ldfp xmm0, seg, riprel, disp, dataSize=8
    movfp xmm1, xmm0, dataSize=8
    movfp xmm2, xmm0, dataSize=8
    movfp xmm3, xmm0, dataSize=8
    movfp xmm4, xmm0, dataSize=8
    movfp xmm5, xmm0, dataSize=8
    movfp xmm6, xmm0, dataSize=8
    movfp xmm7, xmm0, dataSize=8
};

'''