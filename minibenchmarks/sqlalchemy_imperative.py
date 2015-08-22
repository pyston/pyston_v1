import os
import sys
sys.path.append(os.path.join(os.path.dirname(__file__), "../test/lib/sqlalchemy/lib"))

from sqlalchemy import Column, ForeignKey, Integer, String, Table, MetaData
from sqlalchemy.ext.declarative import declarative_base
from sqlalchemy.orm import relationship, sessionmaker
from sqlalchemy import create_engine

metadata = MetaData()

Person = Table('person', metadata,
    Column('id', Integer, primary_key=True),
    Column('name', String(250), nullable=False)
)

Address = Table('address', metadata,
    Column('id', Integer, primary_key=True),
    Column('street_name', String(250)),
    Column('street_number', String(250)),
    Column('post_code', String(250), nullable=False),
    Column('person_id', Integer, ForeignKey('person.id'))
    #person = relationship(Person) XXX
)

# Create an engine that stores data in the local directory's
# sqlalchemy_example.db file.
engine = create_engine('sqlite://')

# Create all tables in the engine. This is equivalent to "Create Table"
# statements in raw SQL.
metadata.create_all(engine) 


# Bind the engine to the metadata of the Base class so that the
# declaratives can be accessed through a DBSession instance
metadata.bind = engine
 
DBSession = sessionmaker(bind=engine)
# A DBSession() instance establishes all conversations with the database
# and represents a "staging zone" for all the objects loaded into the
# database session object. Any change made against the objects in the
# session won't be persisted into the database until you call
# session.commit(). If you're not happy about the changes, you can
# revert all of them back to the last commit by calling
# session.rollback()
session = DBSession()

# add 100 people to the database 
for i in xrange(100):
    # Insert a Person in the person table
    new_person = Person.insert()
    new_person.execute(name="new person %d" % (i,) )
 
    # Insert an Address in the address table
    new_address = Address.insert()
    new_address.execute(post_code='00000')#, person=new_person)

    # do 100 queries per insert
    for i in xrange(100):
        s = Person.select()
        rs = s.execute()

print "done"
