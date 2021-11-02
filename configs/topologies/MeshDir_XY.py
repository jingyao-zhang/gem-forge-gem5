# Copyright (c) 2010 Advanced Micro Devices, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from __future__ import print_function
from __future__ import absolute_import

from m5.params import *
from m5.objects import *

from common import FileSystemConfig

from topologies.BaseTopology import SimpleTopology

import math

# Creates a Mesh topology with directories in the middle/corners/tiled.
# One L1 (and L2, depending on the protocol) are connected to each router.
# XY routing is enforced (using link weights) to guarantee deadlock freedom.

class MeshDir_XY(SimpleTopology):
    description='MeshDir_XY'

    def __init__(self, controllers):
        self.nodes = controllers

    def makeDirCornerTopology(self, ExtLink, dir_nodes, routers, ext_links):
        # NUMA Node for each quadrant
        # With odd columns or rows, the nodes will be unequal
        assert(len(dir_nodes) == 4)
        self.numa_nodes = [[], [], [], []]
        for i in range(self.num_routers):
            if i % self.num_columns < self.num_columns / 2  and \
               i < self.num_routers / 2:
                self.numa_nodes[0].append(i)
            elif i % self.num_columns >= self.num_columns / 2  and \
               i < self.num_routers / 2:
                self.numa_nodes[1].append(i)
            elif i % self.num_columns < self.num_columns / 2  and \
               i >= self.num_routers / 2:
                self.numa_nodes[2].append(i)
            else:
                self.numa_nodes[3].append(i)

        # Connect the dir nodes to the corners.
        ext_links.append(ExtLink(link_id=self.link_count, ext_node=dir_nodes[0],
                                int_node=routers[0],
                                latency=self.link_latency))
        self.link_count += 1
        ext_links.append(ExtLink(link_id=self.link_count, ext_node=dir_nodes[1],
                                int_node=routers[self.num_columns - 1],
                                latency=self.link_latency))
        self.link_count += 1
        ext_links.append(ExtLink(link_id=self.link_count, ext_node=dir_nodes[2],
                                int_node=routers[self.num_routers - self.num_columns],
                                latency=self.link_latency))
        self.link_count += 1
        ext_links.append(ExtLink(link_id=self.link_count, ext_node=dir_nodes[3],
                                int_node=routers[self.num_routers - 1],
                                latency=self.link_latency))
        self.link_count += 1

        self.num_numa_nodes = 0
        for n in self.numa_nodes:
            if n:
                self.num_numa_nodes += 1


    def makeDirMiddleTopology(self, ExtLink, dir_nodes, routers, ext_links):
        num_dir_rows = int(math.sqrt(len(dir_nodes)))
        num_dir_columns = num_dir_rows
        assert(num_dir_rows * num_dir_columns == len(dir_nodes))
        assert(num_dir_rows <= self.num_rows)
        assert(num_dir_columns <= self.num_columns)

        # Connect the dir nodes to the middle.
        middle_start_row = (self.num_rows - num_dir_rows) // 2
        middle_start_col = (self.num_columns - num_dir_columns) // 2
        for i in range(num_dir_rows):
            for j in range(num_dir_columns):
                dir_idx = i * num_dir_columns + j
                router_idx = (i + middle_start_row) * self.num_columns + j + middle_start_col
                print(f'[MeshDirMiddle] Dir {i}x{j} -> Router {i + middle_start_row}x{j + middle_start_col}.')
                ext_links.append(
                    ExtLink(
                        link_id=self.link_count,
                        ext_node=dir_nodes[dir_idx],
                        int_node=routers[router_idx],
                        latency=self.link_latency))
                self.link_count += 1

        # NUMA Node for cloest routers.
        self.numa_nodes = [[]] * len(dir_nodes)
        for i in range(self.num_routers):
            router_row, router_col = divmod(i, self.num_columns)
            min_diff = -1
            min_dir = -1
            for j in range(len(dir_nodes)):
                row, col = divmod(j, num_dir_columns)
                dir_row = row + middle_start_row
                dir_col = col + middle_start_col
                diff = abs(dir_row - router_row) + abs(dir_col - router_col)
                if min_diff == -1 or diff < min_diff:
                    min_dir = j
                    min_diff = diff
            self.numa_nodes[min_dir].append(i)

        self.num_numa_nodes = 0
        for n in self.numa_nodes:
            if n:
                self.num_numa_nodes += 1

    def makeDirTileTopology(self, ExtLink, dir_nodes, routers, ext_links):
        num_dir_rows = int(math.sqrt(len(dir_nodes)))
        num_dir_columns = num_dir_rows
        assert(num_dir_rows * num_dir_columns == len(dir_nodes))
        assert(num_dir_rows <= self.num_rows)
        assert(num_dir_columns <= self.num_columns)

        # Compute tile size.
        num_tile_rows = self.num_rows // num_dir_rows
        num_tile_columns = self.num_columns // num_dir_columns

        # Connect the dir nodes to the top-left tile corner.
        for i in range(num_dir_rows):
            for j in range(num_dir_columns):
                dir_idx = i * num_dir_columns + j
                router_idx = (i * num_tile_rows) * self.num_columns + (j * num_tile_columns) 
                print(f'[MeshDirTile] Dir {i}x{j} -> Router {i * num_tile_rows}x{j * num_tile_columns}.')
                ext_links.append(
                    ExtLink(
                        link_id=self.link_count,
                        ext_node=dir_nodes[dir_idx],
                        int_node=routers[router_idx],
                        latency=self.link_latency))
                self.link_count += 1

        # NUMA Node for routers in the tile.
        self.numa_nodes = [[]] * len(dir_nodes)
        for i in range(self.num_routers):
            router_row, router_col = divmod(i, self.num_columns)
            tile_row = router_row // num_tile_rows
            tile_col = router_col // num_tile_columns
            dir_idx = tile_row * num_dir_columns + tile_col
            print(f'[MeshDirTile] NUMA Router {router_row}x{router_col} -> Dir {tile_row}x{tile_col}.')
            self.numa_nodes[dir_idx].append(i)

        self.num_numa_nodes = 0
        for n in self.numa_nodes:
            if n:
                self.num_numa_nodes += 1

    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        nodes = self.nodes

        self.num_routers = options.num_cpus
        self.num_rows = options.mesh_rows

        # default values for link latency and router latency.
        # Can be over-ridden on a per link/router basis
        self.link_latency = options.link_latency # used by simple and garnet
        router_latency = options.router_latency # only used by garnet


        # First determine which nodes are cache cntrls vs. dirs vs. dma
        cache_nodes = []
        dir_nodes = []
        dma_nodes = []
        for node in nodes:
            if node.type == 'L1Cache_Controller' or \
                node.type == 'L2Cache_Controller' or \
                node.type == 'L0Cache_Controller':
                cache_nodes.append(node)
            elif node.type == 'Directory_Controller':
                dir_nodes.append(node)
            elif node.type == 'DMA_Controller':
                dma_nodes.append(node)
            else:
                print('Unkown node controller {t}'.format(t=node.type))
                assert(False)

        # Obviously the number or rows must be <= the number of routers
        # and evenly divisible.  Also the number of caches must be a
        # multiple of the number of routers and the number of directories
        # must be power of 2.
        assert(self.num_rows > 0 and self.num_rows <= self.num_routers)
        self.num_columns = int(self.num_routers / self.num_rows)
        assert(self.num_columns * self.num_rows == self.num_routers)
        caches_per_router, remainder = divmod(len(cache_nodes), self.num_routers)
        assert(remainder == 0)

        # Create the routers in the mesh
        routers = [Router(router_id=i, latency = router_latency) \
            for i in range(self.num_routers)]
        network.routers = routers

        # link counter to set unique link ids
        self.link_count = 0

        # Connect each cache controller to the appropriate router
        ext_links = []
        for (i, n) in enumerate(cache_nodes):
            cntrl_level, router_id = divmod(i, self.num_routers)
            assert(cntrl_level < caches_per_router)
            ext_links.append(ExtLink(link_id=self.link_count, ext_node=n,
                                    int_node=routers[router_id],
                                    latency=self.link_latency))
            self.link_count += 1

        # Connect the dma nodes to router 0.  These should only be DMA nodes.
        for (i, node) in enumerate(dma_nodes):
            assert(node.type == 'DMA_Controller')
            ext_links.append(ExtLink(link_id=self.link_count, ext_node=node,
                                     int_node=routers[0],
                                     latency=self.link_latency))

        if options.ruby_mesh_dir_location == 'corner':
            self.makeDirCornerTopology(ExtLink, dir_nodes, routers, ext_links)
        elif options.ruby_mesh_dir_location == 'middle':
            self.makeDirMiddleTopology(ExtLink, dir_nodes, routers, ext_links)
        elif options.ruby_mesh_dir_location == 'tile':
            self.makeDirTileTopology(ExtLink, dir_nodes, routers, ext_links)
        else:
            print('Unsupported Dir Location.')
            assert(False)

        network.ext_links = ext_links

        # Smaller weight means higher priority
        weightX = 1
        weightY = 2
        if options.routing_YX:
            weightX = 2
            weightY = 1

        # Create the mesh links.
        int_links = []

        # East output to West input links (weight = 1)
        for row in range(self.num_rows):
            for col in range(self.num_columns):
                if (col + 1 < self.num_columns):
                    east_out = col + (row * self.num_columns)
                    west_in = (col + 1) + (row * self.num_columns)
                    int_links.append(IntLink(link_id=self.link_count,
                                             src_node=routers[east_out],
                                             dst_node=routers[west_in],
                                             src_outport="East",
                                             dst_inport="West",
                                             latency=self.link_latency,
                                             weight=weightX))
                    self.link_count += 1

        # West output to East input links (weight = 1)
        for row in range(self.num_rows):
            for col in range(self.num_columns):
                if (col + 1 < self.num_columns):
                    east_in = col + (row * self.num_columns)
                    west_out = (col + 1) + (row * self.num_columns)
                    int_links.append(IntLink(link_id=self.link_count,
                                             src_node=routers[west_out],
                                             dst_node=routers[east_in],
                                             src_outport="West",
                                             dst_inport="East",
                                             latency=self.link_latency,
                                             weight=weightX))
                    self.link_count += 1

        # North output to South input links (weight = 2)
        for col in range(self.num_columns):
            for row in range(self.num_rows):
                if (row + 1 < self.num_rows):
                    north_out = col + (row * self.num_columns)
                    south_in = col + ((row + 1) * self.num_columns)
                    int_links.append(IntLink(link_id=self.link_count,
                                             src_node=routers[north_out],
                                             dst_node=routers[south_in],
                                             src_outport="North",
                                             dst_inport="South",
                                             latency=self.link_latency,
                                             weight=weightY))
                    self.link_count += 1

        # South output to North input links (weight = 2)
        for col in range(self.num_columns):
            for row in range(self.num_rows):
                if (row + 1 < self.num_rows):
                    north_in = col + (row * self.num_columns)
                    south_out = col + ((row + 1) * self.num_columns)
                    int_links.append(IntLink(link_id=self.link_count,
                                             src_node=routers[south_out],
                                             dst_node=routers[north_in],
                                             src_outport="South",
                                             dst_inport="North",
                                             latency=self.link_latency,
                                             weight=weightY))
                    self.link_count += 1


        network.int_links = int_links


    # Register nodes with filesystem
    def registerTopology(self, options):
        i = 0
        for n in self.numa_nodes:
            if n:
                FileSystemConfig.register_node(n,
                    MemorySize(options.mem_size) // self.num_numa_nodes, i)
            i += 1

