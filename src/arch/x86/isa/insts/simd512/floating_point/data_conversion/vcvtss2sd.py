microcode = '''

def macroop VCVTSS2SD_XMM_XMM {
    cvtf2f xmm0, xmm0m, destSize=8, srcSize=4, ext=Scalar
    movfp  xmm1, xmm1v, dataSize=8
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTSS2SD_XMM_M {
    ldfp ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=4
    cvtf2f xmm0, ufp1, destSize=8, srcSize=4, ext=Scalar
    movfp  xmm1, xmm1v, dataSize=8
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VCVTSS2SD_XMM_P {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=4
    cvtf2f xmm0, ufp1, destSize=8, srcSize=4, ext=Scalar
    movfp  xmm1, xmm1v, dataSize=8
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

'''