import warnings

# Specifying this as a DeprecationWarning is a hacky way of supressing the warning,
# since we don't output the exact same error message as CPython right now:
warnings.warn("hello world", DeprecationWarning)
