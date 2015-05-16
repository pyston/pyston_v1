def print_out(*items):
    print len(items)

class ResultList(list):
    # Wrapper class used to return items in a list_editable
    # changelist, annotated with the form object for error
    # reporting purposes. Needed to maintain backwards
    # compatibility with existing admin templates.
    #def __new__(self):
    #    return super(ResultList, self).__new__(self)

    def __init__(self, form, *items):
        print type(self)
        print form
        print len(items)
        print type(items)
        print len(items[0])

        self.form = form
        print "calling super"
        s = super(ResultList, self)
        print "calling __init__"
        s.__init__(*items)
        print "called super.__init__"

print ResultList(None, (1, 2, 3))
