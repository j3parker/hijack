hijack
======

Assumptions:

1. You have an interactive terminal program.
2. You like that it is interactive.
3. You are considering tee'ing its output to a file so other programs can parse it.
4. You are wondering if there is some way you could create a named pipe that fed into the programs `stdin`.

`hijack` is your tool. You give it a directory to put an `out` file and an `in` named pipe, and some command to run.

This tool can only be used for doing gross things.

Example
-------
In a terminal, type `./hijack foo python`. This appears equivalent to just typing `python` at the moment. Open up another terminal and navigate to the folder `foo`. If you read the `out` file you will have essentially a copy of what is on the screen. If you do `echo "print('Hello, world!')" > in` then it will "type" that into python. Magic.
