from m5.SimObject import SimObject
from m5.params import *


class McPATManager(SimObject):
    type = 'McPATManager'
    cxx_header = 'mcpat/mcpat_manager.hh'
    cxx_class = 'McPATManager'