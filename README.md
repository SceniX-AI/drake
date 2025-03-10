# Drake

Model-Based Design and Verification for Robotics.

Please see the [Drake Documentation](https://drake.mit.edu) for more
information.

#Install
See instruction details [here](https://drake.mit.edu/from_source.html)
```shell
./setup/mac/install_prereqs.sh
```

#Compile on Mac OS
- Compile
```shell
bazel build -c opt --config=clang --define=WITH_NLOPT=ON //...
```
- Install
```shell
bazel run install -c opt --define=WITH_NLOPT=ON -- ~/local
```
