set -x
set -e
set -u

BUILD_NAME=0.5.1

for type in pyston pyston-numpy; do
    for tag in latest ${BUILD_NAME}; do
        cd ~/pyston/docker/${type}
        # "docker build" only recently gained the ability to take multiple -t flags
        docker build -t pyston/${type}:latest ~/pyston/docker/${type}
        docker build -t pyston/${type}:${BUILD_NAME} ~/pyston/docker/${type}
    done
done

for type in pyston pyston-numpy; do
    for tag in latest ${BUILD_NAME}; do
        cd ~/pyston/docker/${type}
        docker push pyston/${type}:latest
        docker push pyston/${type}:${BUILD_NAME}
    done
done
