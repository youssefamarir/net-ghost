Import("env")
# app partition is 4 MB (see partitions.csv)
env.BoardConfig().update("upload.maximum_size", 4194304)
