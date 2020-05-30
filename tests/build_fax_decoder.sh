#!/bin/bash

gcc fax_decoder.c -I ../src -lspandsp -L ../src/.libs/ -lsndfile -o fax_decoder
