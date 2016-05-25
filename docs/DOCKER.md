To produce a new docker build:
```
cd ~/pyston/docker
BUILD_NAME=0.5.0
docker build -t pyston/pyston:latest .
docker build -t pyston/pyston:${BUILD_NAME} .
docker push pyston/pyston:latest
docker push pyston/pyston:${BUILD_NAME}
```

Then using docker is just a matter of `docker run -it pyston/pyston` and running python/pip etc.

Still todo: use PR #1099 to do in-tree builds, rather than just downloading our release?
