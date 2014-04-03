awk '{if ($0 ~ /%/) { s += $1; print s, "\t", $0 } }'
