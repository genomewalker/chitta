terraform {
 required_version = ">= 1.0"
}
variable "sample" {
 type = string
}
resource "local_file" "reads" {
 content = trimspace(var.sample)
 filename = "reads.txt"
}
module "counts" {
 source = "./counts"
 input = local_file.reads.filename
}
output "result" {
 value = module.counts.result
}
