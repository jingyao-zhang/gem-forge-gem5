microcode = '''

def macroop VINSERT256L_ZMM_YMM_I {
    movfp xmm0, xmm0m, dataSize=8
    movfp xmm1, xmm1m, dataSize=8
    movfp xmm2, xmm2m, dataSize=8
    movfp xmm3, xmm3m, dataSize=8
    movfp xmm4, xmm4v, dataSize=8
    movfp xmm5, xmm5v, dataSize=8
    movfp xmm6, xmm6v, dataSize=8
    movfp xmm7, xmm7v, dataSize=8
};

def macroop VINSERT256L_ZMM_M_I {
    ldfp xmm0, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm1, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp xmm2, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp xmm3, seg, sib, "DISPLACEMENT + 24", dataSize=8
    movfp xmm4, xmm4v, dataSize=8
    movfp xmm5, xmm5v, dataSize=8
    movfp xmm6, xmm6v, dataSize=8
    movfp xmm7, xmm7v, dataSize=8
};

def macroop VINSERT256L_ZMM_P_I {
    rdip t7
    ldfp xmm0, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm1, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp xmm2, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp xmm3, seg, riprel, "DISPLACEMENT + 24", dataSize=8
    movfp xmm4, xmm4v, dataSize=8
    movfp xmm5, xmm5v, dataSize=8
    movfp xmm6, xmm6v, dataSize=8
    movfp xmm7, xmm7v, dataSize=8
};

def macroop VINSERT256H_ZMM_YMM_I {
    movfp xmm0, xmm0v, dataSize=8
    movfp xmm1, xmm1v, dataSize=8
    movfp xmm2, xmm2v, dataSize=8
    movfp xmm3, xmm3v, dataSize=8
    movfp xmm4, xmm0m, dataSize=8
    movfp xmm5, xmm1m, dataSize=8
    movfp xmm6, xmm2m, dataSize=8
    movfp xmm7, xmm3m, dataSize=8
};

def macroop VINSERT256H_ZMM_M_I {
    movfp xmm0, xmm0v, dataSize=8
    movfp xmm1, xmm1v, dataSize=8
    movfp xmm2, xmm2v, dataSize=8
    movfp xmm3, xmm3v, dataSize=8
    ldfp xmm4, seg, sib, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm5, seg, sib, "DISPLACEMENT + 8", dataSize=8
    ldfp xmm6, seg, sib, "DISPLACEMENT + 16", dataSize=8
    ldfp xmm7, seg, sib, "DISPLACEMENT + 24", dataSize=8
};

def macroop VINSERT256H_ZMM_P_I {
    movfp xmm0, xmm0v, dataSize=8
    movfp xmm1, xmm1v, dataSize=8
    movfp xmm2, xmm2v, dataSize=8
    movfp xmm3, xmm3v, dataSize=8
    rdip t7
    ldfp xmm4, seg, riprel, "DISPLACEMENT + 0", dataSize=8
    ldfp xmm5, seg, riprel, "DISPLACEMENT + 8", dataSize=8
    ldfp xmm6, seg, riprel, "DISPLACEMENT + 16", dataSize=8
    ldfp xmm7, seg, riprel, "DISPLACEMENT + 24", dataSize=8
};

'''