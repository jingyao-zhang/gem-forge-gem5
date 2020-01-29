microcode = '''
def macroop VMOVSD_XMM_XMM {
    panic "VMOVSD_XMM_XMM not implemented."
};

def macroop VMOVSD_XMM_M {
    ldfp xmm0, seg, sib, disp, dataSize=8
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMOVSD_XMM_P {
    rdip t7
    ldfp xmm0, seg, riprel, disp, dataSize=8
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMOVSD_M_XMM {
    stfp xmm0, seg, sib, disp, dataSize=8
};

def macroop VMOVSD_P_XMM {
    rdip t7
    stfp xmm0, seg, riprel, disp, dataSize=8
};

def macroop VMOVSD_ZMM_ZMM {
    panic "VMOVSD_ZMM_ZMM is not implemented."
};

def macroop VMOVSD_ZMM_M {
    ldfp xmm0, seg, sib, disp, dataSize=8
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

def macroop VMOVSD_ZMM_P {
    rdip t7
    ldfp xmm0, seg, riprel, disp, dataSize=8
    lfpimm xmm1, 0
    lfpimm xmm2, 0
    lfpimm xmm3, 0
    lfpimm xmm4, 0
    lfpimm xmm5, 0
    lfpimm xmm6, 0
    lfpimm xmm7, 0
};

'''