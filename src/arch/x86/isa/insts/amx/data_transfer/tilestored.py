
microcode = """

def macroop TILESTORED_ZMM_M {
    stfp512 tmm0, seg, sib_tile_row0, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm8, seg, sib_tile_row1, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm16, seg, sib_tile_row2, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm24, seg, sib_tile_row3, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm32, seg, sib_tile_row4, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm40, seg, sib_tile_row5, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm48, seg, sib_tile_row6, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm56, seg, sib_tile_row7, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm64, seg, sib_tile_row8, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm72, seg, sib_tile_row9, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm80, seg, sib_tile_row10, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm88, seg, sib_tile_row11, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm96, seg, sib_tile_row12, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm104, seg, sib_tile_row13, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm112, seg, sib_tile_row14, "DISPLACEMENT + 0", dataSize=64
    stfp512 tmm120, seg, sib_tile_row15, "DISPLACEMENT + 0", dataSize=64
};

def macroop TILESTORED_ZMM_ZMM {
    panic "TILELOADD with Reg operand in R/M"
};

def macroop TILESTORED_ZMM_P {
    panic "TILELOADD with P operand in R/M"
};

"""
