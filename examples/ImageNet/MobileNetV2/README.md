# MobileNet V2 example

This folder demonstrates an example of inference on MobileNetV2.
Note: this is a work in progress, and requires ~50GB memory.
Runtime will be very slow without many cores.

See https://github.com/tensorflow/models/tree/master/research/slim/nets/mobilenet
for a description.

# Setup
1. Make sure python env is active, i.e. run
```bash
source $HE_TRANSFORMER/build/external/venv-tf-py3/bin/activate
```
Also ensure the `pyhe_client` wheel has been installed (see `python` folder for instructions).

The examples rely on numpy and pillow, so run
```bash
pip install numpy pillow
```

2. Build Tensorflow graph transforms and add them to your path:

To build run:
```bash
cd $HE_TRANSFORMER/build/ext_ngraph_tf/src/ext_ngraph_tf/build_cmake/tensorflow
bazel build tensorflow/tools/graph_transforms:transform_graph
```

To add to path run:
```bash
export PATH=$HE_TRANSFORMER/build/ext_ngraph_tf/src/ext_ngraph_tf/build_cmake/tensorflow/bazel-bin/tensorflow/tools/graph_transforms:$PATH
```

3. To download the models and optimize for inference, call
```bash
python get_models.py
```

# Image-Net evaluation
1. First, sign up for an account at image-net.org
2. Download the 2012 validation_images (all tasks)) 13GB MD5: `29b22e2961454d5413ddabcf34fc5622` file on image-net.org

Extract the validation images:
```bash
tar -xf ILSVRC2012_img_val.tar
```
3. Download development kit (Task 1 & 2) and extract `validation_ground_truth.txt`

The directory setup should be:
```
DATA_DIR/validation_images/ILSVRC2012_val_00000001.JPEG
DATA_DIR/validation_images/ILSVRC2012_val_00000002.JPEG
...
DATA_DIR/validation_images/ILSVRC2012_val_00050000.JPEG
DATA_DIR/ILSVRC2012_validation_ground_truth.txt
```
for some `DATA_DIR` folder.

For the remaining instructions, run```bash
export DATA_DIR=path_to_your_data_dir
```

## CPU backend
To run inference using the CPU backend on unencrypted data, call
```bash
python test.py \
  --data_dir=$DATA_DIR \
  --batch_size=300 \
  --backend=CPU
```

5. To call inference using HE_SEAL's plaintext operations (for debugging), call
```bash
python test.py \
--data_dir=$DATA_DIR \
--batch_size=300 \
--backend=HE_SEAL
```
  5.a To try on a larger model, call:
  ```bash
  python test.py \
  --image_size=128 \
  --data_dir=$DATA_DIR \
  --batch_size=30 \
  --model=./model/mobilenet_v2_0.35_128_opt.pb \
  --ngraph=true \
  --backend=HE_SEAL
  ```

6. To call inference using encrypted data, run the below command. ***Warning***: this requires ~50GB memory.
```bash
OMP_NUM_THREADS=56 \
python test.py \
--data_dir=$DATA_DIR \
--ngraph=true \
--batch_size=2048 \
--encrypt_server_data=true \
--backend=HE_SEAL \
--encryption_parameters=$HE_TRANSFORMER/configs/he_seal_ckks_config_N12_L4.json
```

6a. To try on a larger model, call:
  ```bash
  OMP_NUM_THREADS=56 \
  python test.py \
  --image_size=128 \
  --data_dir=$DATA_DIR \
  --ngraph=true \
  --model=./model/mobilenet_v2_0.35_128_opt.pb \
  --encryption_parameters=$HE_TRANSFORMER/configs/he_seal_ckks_config_N12_L4.json \
  --batch_size=30 \
  --backend=HE_SEAL \
  --encrypt_server_data=true
  ```

7. To double the throughput using complex packing, run the below command.  ***Warning***: this requires ~120GB memory.
```bash
OMP_NUM_THREADS=56 \
python test.py \
--data_dir=$DATA_DIR \
--ngraph=true \
--encryption_parameters=$HE_TRANSFORMER/configs/he_seal_ckks_config_N12_L4_complex.json \
--batch_size=4096 \
--backend=HE_SEAL  \
--encrypt_server_data=true
```

# TODO: remove batch size argument
8. To enable the client, in one terminal, run:
```bash
OMP_NUM_THREADS=56 \
NGRAPH_HE_VERBOSE_OPS=BoundedRelu \
NGRAPH_HE_LOG_LEVEL=3 \
python test.py \
  --batch_size=10  \
  --image_size=96 \
  --ngraph=true \
  --model=./model/mobilenet_v2_0.35_96_opt.pb \
  --data_dir=$DATA_DIR \
  --backend=HE_SEAL  \
  --ngraph=true \
  --enable_client=yes \
  --encryption_parameters=$HE_TRANSFORMER/configs/he_seal_ckks_config_N12_L4_complex.json
```
Since this will take a while to run, we have added verbosity flags to the above command, e.g. `NGRAPH_HE_VERBOSE_OPS=all NGRAPH_HE_LOG_LEVEL=3`

In another terminal (with the python environment active), run
```bash
OMP_NUM_THREADS=56 \
python client.py \
  --batch_size=10 \
  --image_size=96 \
  --data_dir=$DATA_DIR
```
