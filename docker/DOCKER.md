### For Pyston developers

To produce a new docker build, do `bash build_docker.sh`.  You will need to do `docker login` first.

Then using docker is just a matter of `docker run -it pyston/pyston` and running python/pip etc.

Still todo: use PR #1099 to do in-tree builds, rather than just downloading our release?
