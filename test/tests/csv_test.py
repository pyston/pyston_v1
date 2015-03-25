import csv
import StringIO

csvfile = StringIO.StringIO()

fieldnames = ['first_name', 'last_name']
writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
writer.writeheader()
writer.writerow({'first_name': 'Baked', 'last_name': 'Beans'})
writer.writerow({'first_name': 'Lovely', 'last_name': 'Spam'})
writer.writerow({'first_name': 'Wonderful', 'last_name': 'Spam'})

csvfile.seek(0)
print csvfile.getvalue()
reader = csv.DictReader(csvfile)
for row in reader:
    print row['first_name'], row['last_name']
