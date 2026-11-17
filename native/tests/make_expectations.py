# Prints Python's own repr(float) and json.dumps(indent=2) results as C++
# test cases (tests/test_pytext.cpp embeds this output).
import json

values = [0.0, -0.0, 1.0, 0.5, -2.0, 1e-05, 0.0001, 123456789.0, 1e16, 1e15, 1.5e+20, 0.1, 1 / 3,
          2.5e-300, 65504.0, 1e22, 12345678901234567.0, -1e-7, 0.00012, 3.14159]
for v in values:
    print(f'    {{"{v.hex()}", "{repr(v)}"}},')

doc = {'a': [1, {'b': 'xé\x7f"\\'}], 'c': {}, 'd': [], 'e': 0.5, 'f': True, 'g': None,
       'h': '\U0001F600', 'i': -3}
print(json.dumps(json.dumps(doc, indent=2)))
