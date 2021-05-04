import torch

import sys
if len(sys.argv) == 2 and sys.argv[1] == '--torchy':
  import torchy
  torchy.enable()
elif len(sys.argv) != 1:
  print('Unknown options:', sys.argv)
  exit(-1)
