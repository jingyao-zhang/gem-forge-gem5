microcode = '''
def macroop VMOVSS_XMM_XMM {
    panic "VMOVSS_XMM_XMM not implemented."
};

def macroop VMOVSS_XMM_M {
    lfpimm xmm0, 0
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
    ldfp xmm0, seg, sib, disp, dataSize=4
};

def macroop VMOVSS_XMM_P {
    rdip t7
    lfpimm xmm0, 0
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
    ldfp xmm0, seg, riprel, disp, dataSize=4
};


def macroop VMOVSS_M_XMM {
    stfp xmm0, seg, sib, disp, dataSize=4
};

def macroop VMOVSS_P_XMM {
    rdip t7
    stfp xmm0, seg, riprel, disp, dataSize=4
};

def macroop VMOVSS_ZMM_ZMM {
    panic "VMOVSS_ZMM_ZMM is not implemented."
};

def macroop VMOVSS_ZMM_M {
    lfpimm xmm0, 0
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
    ldfp xmm0, seg, sib, disp, dataSize=4
};

def macroop VMOVSS_ZMM_P {
    rdip t7
    lfpimm xmm0, 0
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
    ldfp xmm0, seg, riprel, disp, dataSize=4
};

'''