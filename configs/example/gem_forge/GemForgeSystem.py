import m5

def run(options, root, system, future_cpus):
    checkpoint_dir = None
    # We only allow some number of maximum instructions in real simulation.
    # In total, 4 billion instructions.
    if future_cpus:
        future_cpus[0].max_insts_any_thread = 1e9
    m5.instantiate(checkpoint_dir)
    max_tick = options.abs_max_tick if options.abs_max_tick else m5.MaxTick
    if future_cpus:
        assert(len(future_cpus) == len(system.cpu))
        # Fast forward simulation.
        print('**** FAST FORWARD SIMULATION ****')
        exit_event = m5.simulate(max_tick)
        print('**** Switch CPUS @ tick {t} as {s} ****'.format(
            t=m5.curTick(), s=exit_event.getCause()))

        switch_cpu_list = [(system.cpu[i], future_cpus[i]) \
            for i in range(len(system.cpu))]
        m5.switchCpus(system, switch_cpu_list)
        m5.stats.reset()
    print('**** REAL SIMULATION ****')
    assert(max_tick > m5.curTick())
    exit_event = m5.simulate(max_tick - m5.curTick())
    print('**** Exit @ tick {t} as {s} ****'.format(
        t=m5.curTick(), s=exit_event.getCause()))
    if exit_event.getCode() != 0:
        print('Simulated exit code ({s})'.format(
            s=exit_event.getCode()))


