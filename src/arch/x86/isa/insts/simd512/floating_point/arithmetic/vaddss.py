
microcode = '''
def macroop VADDSS_XMM_XMM {
    movfp dest=xmm0, src1=xmm0v, dataSize=8
    movfp dest=xmm1, src1=xmm1v, dataSize=8
    maddf xmm0, xmm0v, xmm0m, size=4, ext=Scalar
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VADDSS_XMM_M {
    movfp dest=xmm0, src1=xmm0v, dataSize=8
    movfp dest=xmm1, src1=xmm1v, dataSize=8
    ldfp ufp1, seg, sib, disp, dataSize=4
    maddf xmm0, xmm0v, ufp1, size=4, ext=Scalar
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VADDSS_XMM_P {
    movfp dest=xmm0, src1=xmm0v, dataSize=8
    movfp dest=xmm1, src1=xmm1v, dataSize=8
    rdip t7
    ldfp ufp1, seg, sib, disp, dataSize=4
    maddf xmm0, xmm0v, ufp1, size=4, ext=Scalar
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

'''
