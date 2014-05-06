def f():
    with a as b:
        try:
            raise 1
        except (c, d) as e:
            pass
        except:
            pass
        else:
            1
        finally:
            pass
# Note: don't call f
