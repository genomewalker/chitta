include: "rules/common.smk"
module reference:
    snakefile: "modules/reference/Snakefile"
def collect(source, target):
    return source
rule align:
    input: "reads/{sample}.fq"
    output: "aligned/{sample}.bam"
    params: threads=4
    shell: "align {input} > {output}"
rule count:
    input: rules.align.output
    output: "counts.txt"
    run:
        collect(input[0], output[0])
