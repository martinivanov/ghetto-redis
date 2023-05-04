#!/usr/bin/env python3

input_file = "input.txt"
output_file = "output.txt"

with open(input_file, "r", newline='') as infile, open(output_file, "w", newline='\r\n') as outfile:
    for line in infile:
        outfile.write(line)
