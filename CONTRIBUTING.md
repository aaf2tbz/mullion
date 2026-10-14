# Contributing

Bug reports help the most right now. The README says what to put in one.

For a patch:

- Add a test for what you change, and say where the behaviour is specified: the Direct3D specification, Microsoft's documentation or Apple's.
- Run the suite, `python3 tests/run.py --build <build> --wine <wine> --family native,9`, and say which Mac you ran it on.
- Keep it small. A fix should hold for every game that does the same thing, not for one title.
