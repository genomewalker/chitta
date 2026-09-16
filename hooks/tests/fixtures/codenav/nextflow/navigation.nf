include { REPORT } from './modules/report.nf'
process ALIGN {
    input:
    val reads
    output:
    stdout
    script:
    """
    echo $reads
    """
}
process COUNT {
    input:
    val aligned
    output:
    stdout
    script:
    """
    echo $aligned
    """
}
def label(x) { return x }
workflow PIPELINE {
    reads = Channel.of('sample')
    ALIGN(reads)
    COUNT(ALIGN.out)
}
workflow {
    Channel.of('sample') | ALIGN | COUNT
}
// process phantom { }
