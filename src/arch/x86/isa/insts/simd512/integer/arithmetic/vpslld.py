microcode = '''

def macroop VPSLLD_ZMM_I {
    vshiftll dest=xmm0v, src=xmm0m, imm8="IMMEDIATE", size=4, VL=64
};

def macroop VPSLLD_M_I {
    ldfp512 ufp1, seg, sib, "DISPLACEMENT + 0", dataSize=64
    vshiftll dest=xmm0v, src=ufp1, imm8="IMMEDIATE", size=4, VL=64
};

def macroop VPSLLD_P_I {
    rdip t7
    ldfp512 ufp1, seg, riprel, "DISPLACEMENT + 0", dataSize=64
    vshiftll dest=xmm0v, src=ufp1, imm8="IMMEDIATE", size=4, VL=64
};
'''