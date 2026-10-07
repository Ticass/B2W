"""Standalone All2Raw executable and source command."""
import argparse
from dataclasses import replace

from .launcher import ProcessRunner, Settings, discover, perform_extract_all


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='Extract all installed WaW/BO2 zones into a reusable shared cache.')
    for name in ('waw', 'bo2', 't4', 't6', 'work'):
        parser.add_argument('--' + name, help='override the saved ' + name + ' path')
    parser.add_argument('--map', dest='fastfile', help='also prepare a custom map and its companion fastfiles')
    parser.add_argument('--verbose', action='store_true', default=None, help='stream native tool output')
    args = parser.parse_args(argv)
    settings = discover(Settings.load())
    settings = replace(settings, **{k: v for k, v in vars(args).items() if v is not None})
    runner = ProcessRunner(lambda event, value: print(value, flush=True), verbose=settings.verbose)
    try:
        return perform_extract_all(settings, runner)
    except Exception as error:
        print('All2Raw: ' + str(error), flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
