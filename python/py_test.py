import numpy as np
h_l_bias = np.array(([1, 0.39558, 0.75548],
                [1, 0.47145, 0.58025],
                [1, 0.77841, 0.70603],
                [1, 0.50746, 0.71304]))
h_l = h_l_bias[:,1:]

ce = np.array(([-0.50135, 0.50135],
               [-0.50174, 0.50174],
               [0.49747, -0.49747],
               [0.49828, -0.49828]))

#print(np.multiply(temp.T, ce))

yhat=np.array(([0.49865, 0.50135],
               [0.49826, 0.50174],
               [0.49747, 0.50253],
               [0.49828, 0.50172]))
y = np.array(([1, 0],
              [1, 0],
              [0, 1],
              [0, 1]))
print(np.dot((h_l_bias).T, yhat-y))
print(h_l_bias.T)
print()
print(yhat-y)
