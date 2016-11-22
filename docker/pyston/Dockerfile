# Adapted from CPython's Dockerfile:

FROM buildpack-deps:jessie

# http://bugs.python.org/issue19846
# > At the moment, setting "LANG=C" on a Linux system *fundamentally breaks Python 3*, and that's not OK.
ENV LANG C.UTF-8

ENV PYSTON_TAG v0.7.0
ENV PYSTON_VERSION 0.7.0

RUN set -x \
    && curl -SL "https://github.com/dropbox/pyston/releases/download/${PYSTON_TAG}/pyston-${PYSTON_VERSION}-linux64.tar.gz" | tar -xzC / \
    && /pyston-${PYSTON_VERSION}-linux64/pyston /pyston-${PYSTON_VERSION}-linux64/virtualenv/virtualenv.py /pyston_env

ENV PATH /pyston_env/bin/:$PATH

RUN pip install https://github.com/dropbox/pyston/releases/download/${PYSTON_TAG}/Cython-0.24-pyston.tar.gz

RUN echo "source /pyston_env/bin/activate" >> /root/.bashrc

CMD ["/bin/bash"];
