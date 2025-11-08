#!/bin/bash

if [ $RUN_MODE = "h2" ]; then
	echo "RUN MODE : $RUN_MODE"
	/app/h2server -p $PORT -n $THREADS
else
	echo "RUN MODE : h2c"
	/app/h2server -p $PORT -n $THREADS --h2c
fi
