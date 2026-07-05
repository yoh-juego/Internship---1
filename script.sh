#!/bin/bash

echo "Enter C program name (without .c):"
read c_name

echo "Enter Python program name (without .py):"
read py_name

gcc $c_name.c -o $c_name
./$c_name | python3 $py_name.py
