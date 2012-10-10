
My info guide.

How to cherry pick commits from other branch to yours.

fetch new branch, checkout inside.
now git log
find the hash number of the oldest commit that you want and the newst commit that you want,
it's a range of commits! :)

now example! ( the .. from old to new is a must)
rm -f *.patch  (clean all old junk)

you must use commit hash -1 from commot that you want to start adding!
just take commit that you have/not want as first, git format-patch will start from next to one you put as the oldest.!

git format-patch -1 OLD 177e5c7ce53b6d06b9ee3448c00215ba6d70ffc9..NEWc87ade04d28d2024b8ed2000346aa568a07a7f0b
git checkout DESTINATION branch.
git am *.patch
rm -f *.patch
git push

all done :)

More will be added later.
