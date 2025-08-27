# Formocast: A hardware simulation-based prediction model for tensilelite GEMM algorithms

Formocast is a prediction model which forecast the performance of tensilelite solutions.
An example is in path: /rocm-libraries_p/shared/formocast/example/BBSTN.yaml
The parameter PredictionThreshold is to control the number of solutions to be run when tuning.

PredictionThreshold is from 0.0~1.0

build the tensilelite client and run with BBSTN.yaml. Check the log and the forecast result.
1. Go to the tensilelite folder.
2. Run command "invoke build-client"
3. Run command "./build_tmp/Tensile.sh ../../../shared/formocast/example/BBSTN.yaml build"
4. Check the log. It should show the selected solution only.