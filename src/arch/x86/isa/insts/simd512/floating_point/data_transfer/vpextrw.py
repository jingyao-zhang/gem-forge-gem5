microcode = '''

def macroop VPEXTRW_R_XMM_I {
    vextract reg, xmm0m, imm8="IMMEDIATE", srcVL=16, destVL=8, size=2
};

def macroop VPEXTRW_M_XMM_I {
    vextract t1, xmm0m, imm8="IMMEDIATE", srcVL=16, destVL=8, size=2
    stfp t1, seg, sib, disp, dataSize=2
};

def macroop VPEXTRW_P_XMM_I {
    rdip t7
    vextract t1, xmm0m, imm8="IMMEDIATE", srcVL=16, destVL=8, size=2
    stfp t1, seg, riprel, disp, dataSize=2
};

'''