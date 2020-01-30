microcode = '''

def macroop VMOVD_R_XMM {
    mov2int reg, xmm0m, size=dsz
};

def macroop VMOVD_M_XMM {
    stfp xmm0, seg, sib, disp, dataSize=dsz
};

def macroop VMOVD_P_XMM {
    rdip t7
    stfp xmm0, seg, riprel, disp, dataSize=dsz
};

'''