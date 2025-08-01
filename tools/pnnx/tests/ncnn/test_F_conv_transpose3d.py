# Copyright 2022 Tencent
# SPDX-License-Identifier: BSD-3-Clause

import torch
import torch.nn as nn
import torch.nn.functional as F

class Model(nn.Module):
    def __init__(self):
        super(Model, self).__init__()

        self.w2 = nn.Parameter(torch.rand(6, 12, 4, 4, 4))
        self.b2 = nn.Parameter(torch.rand(12))
        self.w3 = nn.Parameter(torch.rand(12, 2, 3, 3, 3))

    def forward(self, y):
        y = F.conv_transpose3d(y, self.w2, self.b2, stride=(2,2,2), padding=(1,0,1), output_padding=(1,1,0))
        y = F.conv_transpose3d(y, self.w3, None, stride=(1,1,2), padding=(2,2,1), dilation=(2,2,1), groups=3)
        return y

def test():
    net = Model().half().float()
    net.eval()

    torch.manual_seed(0)
    y = torch.rand(1, 6, 4, 5, 6)

    a = net(y)

    # export torchscript
    mod = torch.jit.trace(net, y)
    mod.save("test_F_conv_transpose3d.pt")

    # torchscript to pnnx
    import os
    os.system("../../src/pnnx test_F_conv_transpose3d.pt inputshape=[1,6,4,5,6]")

    # ncnn inference
    import test_F_conv_transpose3d_ncnn
    b = test_F_conv_transpose3d_ncnn.test_inference()

    return torch.allclose(a, b, 1e-4, 1e-4)

if __name__ == "__main__":
    if test():
        exit(0)
    else:
        exit(1)
