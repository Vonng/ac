#!/bin/bash
# REQUIRES SUDO
# Benchmark runner

repeats=20
output_file='/ramdisk/bench/results.csv'
command_to_run='echo 1'

run_tests() {
    # --------------------------------------------------------------------------
    # Benchmark loop
    # --------------------------------------------------------------------------
    echo 'Benchmarking ' $command_to_run '...';
    # Indicate the command we just run in the csv file
    echo '======' $command_to_run '======' >> $output_file;

    # Run the given command [repeats] times
    for (( i = 1; i <= $repeats ; i++ ))
    do
        # percentage completion
        p=$(( $i * 100 / $repeats))
        # indicator of progress
        l=$(seq -s "+" $i | sed 's/[0-9]//g')

        rm -f /ramdisk/bench/tmp/*
        # runs time function for the called script, output in a comma seperated
        # format output file specified with -o command and -a specifies append
        # /usr/bin/time -f "%E,%U,%S" -o ${output_file} -a ${command_to_run} > /dev/null 2>&1
        GOGC=off chrt -f 99 /usr/bin/time -f "%e" -o ${output_file} -a ${command_to_run} > /dev/null 2>&1

        # Clear the HDD cache (I hope?)
        sync && echo 3 > /proc/sys/vm/drop_caches

        echo -ne ${l}' ('${p}'%) \r'
    done;
    md5sum -c <<<"5d76461b53079d20c08eb0b33c46b7cd  /ramdisk/bench/tmp/result"

    echo -ne '\n'

    # Convenience seperator for file
    echo '--------------------------' >> $output_file
}

# Option parsing
while getopts n:c:o: OPT
do
    case "$OPT" in
        n)
            repeats=$OPTARG
            ;;
        o)
            output_file=$OPTARG
            ;;
        c)
            command_to_run=$OPTARG
            run_tests
            ;;
        \?)
            echo 'No arguments supplied'
            exit 1
            ;;
    esac
done

shift `expr $OPTIND - 1`
