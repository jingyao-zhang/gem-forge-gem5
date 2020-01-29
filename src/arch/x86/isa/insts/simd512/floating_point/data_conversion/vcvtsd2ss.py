microcode = '''

def macroop VCVTSD2SS_XMM_XMM {
    cvtf2f xmm0, xmm0m, destSize=4, srcSize=8, ext=Scalar
    movfph32 xmm0, xmm0v, dataSize=4
    movfp  xmm1, xmm1v, dataSize=8
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTSD2SS_XMM_M {
    ldfp ufp1, seg, sib, disp, dataSize=8
    cvtf2f xmm0, ufp1, destSize=4, srcSize=8, ext=Scalar
    movfph32 xmm0, xmm0v, dataSize=4
    movfp  xmm1, xmm1v, dataSize=8
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTSD2SS_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, disp, dataSize=8
    cvtf2f xmm0, ufp1, destSize=4, srcSize=8, ext=Scalar
    movfph32 xmm0, xmm0v, dataSize=4
    movfp  xmm1, xmm1v, dataSize=8
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

'''